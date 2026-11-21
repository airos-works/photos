#include "PlainHttp.h"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace photos {

static HttpResponse
PerformOnce(const HttpRequest& request)
{
	HttpResponse response;
	std::string url = request.url;
	if (url.compare(0, 7, "http://") != 0) {
		response.error = "only http:// is supported";
		return response;
	}
	std::string rest = url.substr(7);
	size_t slash = rest.find('/');
	std::string hostPort = slash == std::string::npos ? rest : rest.substr(0, slash);
	std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
	std::string host = hostPort, port = "80";
	size_t colon = hostPort.rfind(':');
	if (colon != std::string::npos) {
		host = hostPort.substr(0, colon);
		port = hostPort.substr(colon + 1);
	}
	addrinfo hints = {};
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* result = nullptr;
	if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) {
		response.error = "cannot resolve";
		return response;
	}
	int fd = socket(result->ai_family, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, result->ai_addr, result->ai_addrlen) != 0) {
		freeaddrinfo(result);
		if (fd >= 0)
			close(fd);
		response.error = "cannot connect";
		return response;
	}
	freeaddrinfo(result);
	// A multipart body is built in memory (tests send small files).
	HttpRequest built = request;
	if (!request.multipart.empty()) {
		std::string boundary = "----photos-test-boundary-7d3f9a";
		for (auto& part : request.multipart) {
			built.body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\""
				+ part.name + "\"";
			std::string value = part.value;
			if (!part.filePath.empty()) {
				built.body += "; filename=\"" + (part.fileName.empty()
					? std::string("file") : part.fileName) + "\"";
				FILE* file = fopen(part.filePath.c_str(), "rb");
				value.clear();
				if (file != nullptr) {
					char chunk[65536];
					size_t got;
					while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0)
						value.append(chunk, got);
					fclose(file);
				}
			}
			built.body += "\r\n";
			if (!part.contentType.empty())
				built.body += "Content-Type: " + part.contentType + "\r\n";
			built.body += "\r\n" + value + "\r\n";
		}
		built.body += "--" + boundary + "--\r\n";
		built.contentType = "multipart/form-data; boundary=" + boundary;
	}
	const HttpRequest& request_ = built;
	std::string head = request.method + " " + path + " HTTP/1.1\r\nHost: " + hostPort
		+ "\r\nConnection: close\r\nUser-Agent: Photos-tests\r\n";
	if (!request.bearer.empty())
		head += "Authorization: Bearer " + request.bearer + "\r\n";
	else if (!request.user.empty())
		head += "Authorization: Basic " + Base64Encode(request.user + ":"
			+ request.password) + "\r\n";
	if (!request_.contentType.empty())
		head += "Content-Type: " + request_.contentType + "\r\n";
	for (auto& header : request.headers)
		head += header.first + ": " + header.second + "\r\n";
	if (!request_.body.empty() || request.method == "POST" || request.method == "PUT"
		|| request.method == "PATCH")
		head += "Content-Length: " + std::to_string(request_.body.size()) + "\r\n";
	head += "\r\n";
	std::string data = head + request_.body;
	size_t sent = 0;
	while (sent < data.size()) {
		ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
		if (n <= 0)
			break;
		sent += n;
	}
	std::string raw;
	char buffer[65536];
	ssize_t n;
	while ((n = recv(fd, buffer, sizeof(buffer), 0)) > 0)
		raw.append(buffer, n);
	close(fd);
	size_t end = raw.find("\r\n\r\n");
	if (end == std::string::npos) {
		response.error = "bad response";
		return response;
	}
	std::string headers = raw.substr(0, end);
	std::string body = raw.substr(end + 4);
	response.status = atol(headers.c_str() + 9);
	size_t pos = headers.find("\r\n");
	while (pos != std::string::npos) {
		size_t next = headers.find("\r\n", pos + 2);
		std::string line = headers.substr(pos + 2, next == std::string::npos
			? std::string::npos : next - pos - 2);
		size_t c = line.find(':');
		if (c != std::string::npos) {
			std::string name = line.substr(0, c);
			std::transform(name.begin(), name.end(), name.begin(), ::tolower);
			std::string value = line.substr(c + 1);
			value.erase(0, value.find_first_not_of(" \t"));
			response.headers[name] = value;
		}
		pos = next;
	}
	if (response.Header("transfer-encoding").find("chunked") != std::string::npos) {
		std::string decoded;
		size_t at = 0;
		while (at < body.size()) {
			size_t lineEnd = body.find("\r\n", at);
			if (lineEnd == std::string::npos)
				break;
			size_t size = strtoul(body.substr(at, lineEnd - at).c_str(), nullptr, 16);
			if (size == 0)
				break;
			decoded += body.substr(lineEnd + 2, size);
			at = lineEnd + 2 + size + 2;
		}
		body = decoded;
	}
	response.url = request.url;
	if (response.Ok() && !request.downloadPath.empty()) {
		FILE* file = fopen(request.downloadPath.c_str(), "wb");
		if (file == nullptr || fwrite(body.data(), 1, body.size(), file) != body.size())
			response.error = "cannot write " + request.downloadPath;
		if (file != nullptr)
			fclose(file);
		if (!response.error.empty())
			response.status = 0;
		return response;
	}
	if (response.Ok() && request.onBody) {
		request.onBody(body.data(), body.size());
		return response;
	}
	response.body = body;
	return response;
}


HttpResponse
PlainHttp::Perform(const HttpRequest& request, const std::atomic<bool>* cancelled)
{
	HttpRequest current = request;
	for (int hop = 0; hop < 5; hop++) {
		requests++;
		HttpResponse response = PerformOnce(current);
		if (current.followRedirects && (response.status == 301 || response.status == 302
				|| response.status == 307 || response.status == 308)
			&& !response.Header("location").empty()) {
			current.url = ResolveUrl(current.url, response.Header("location"));
			continue;
		}
		response.url = current.url;
		return response;
	}
	HttpResponse failed;
	failed.error = "too many redirects";
	return failed;
}

}	// namespace photos
