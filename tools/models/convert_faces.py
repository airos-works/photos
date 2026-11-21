#!/usr/bin/env python3
"""Makes the face networks of Photos from the files their authors publish.

    convert_faces.py [--work DIR] [--out DIR] [--check]

Downloads (once, to DIR, checked by SHA-256) from pinned commits of
opencv_zoo:

    face_detection_yunet_2023mar.onnx      YuNet, the face detector  (MIT)
    face_recognition_sface_2021dec.onnx    SFace, face embeddings    (Apache-2.0)

and writes TensorFlow Lite files the air/OS Neural Kit runs, on the CPU and
whole on a graphics processor (the Vulkan back end):

    models/face_detector_640.tflite    YuNet for a 640 x 640 picture
    models/face_detector_1280.tflite   the same weights for 1280 x 1280
    models/face_embedder.tflite        SFace

Neither TensorFlow nor PyTorch is needed: the ONNX graphs are small (plain
convolutions) and are rewritten operator by operator:

  - pictures are NHWC instead of NCHW; weights are transposed to match;
  - batch normalisations go into the convolution (or fully connected layer)
    before or after them;
  - a ReLU after a convolution becomes its fused activation;
  - a convolution that ONNX pads differently from TensorFlow's SAME gets an
    explicit PAD in front and VALID padding;
  - PReLU, which the Vulkan back end does not have, becomes
    a * x + relu((1 - a) * x)  (two MULs and an ADD) where every slope a of
    the layer is below 1, and relu(x) - a * relu(-x) (one operator more) in
    the two layers of SFace where some are not;
  - YuNet's nearest-neighbour upsampling by two, which the Vulkan back end
    does not have either, becomes two CONCATENATIONs with a RESHAPE each
    (every value twice along the row, then every row twice);
  - YuNet's outputs keep their names and shapes ([1, cells, n], cells in
    row order); the detector's input is BGR, 0..255, as the original's;
  - SFace's own input arithmetic ((x - 127.5) / 128) is left to the caller
    and its first convolution is given BGR instead of RGB, so that both
    networks take the bytes of a B_RGB32 bitmap in order: the embedder's
    input is (BGR - 127.5) / 128. Its output is the 128 numbers of the
    original (not normalised).

--check runs each network with ONNX Runtime and with LiteRT on the same
random input and prints the largest difference.

Needs: pip install numpy onnx flatbuffers ai-edge-litert (and onnxruntime
for --check).
"""

import argparse
import hashlib
import os
import sys
import urllib.request

import flatbuffers
import numpy as np
import onnx
from onnx import numpy_helper
from ai_edge_litert import schema_py_generated as schema

Op = schema.BuiltinOperator

ZOO = ('https://github.com/opencv/opencv_zoo/raw/'
    '25f423d0e04c31a17254620e58febd7386da523b/models/')
SOURCES = {
    'face_detection_yunet_2023mar.onnx': (
        ZOO + 'face_detection_yunet/face_detection_yunet_2023mar.onnx',
        '8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4'),
    'face_recognition_sface_2021dec.onnx': (
        ZOO + 'face_recognition_sface/face_recognition_sface_2021dec.onnx',
        '0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79'),
}


def fetch(name, work):
    url, digest = SOURCES[name]
    path = os.path.join(work, name)
    if not os.path.exists(path):
        print('downloading', url)
        urllib.request.urlretrieve(url, path + '.part')
        os.rename(path + '.part', path)
    with open(path, 'rb') as file:
        found = hashlib.sha256(file.read()).hexdigest()
    if found != digest:
        sys.exit('%s: SHA-256 %s, expected %s' % (path, found, digest))
    return path


# -- TensorFlow Lite files --------------------------------------------------

class Graph:
    """A network being put together. Tensors are numbers; ops return the
    tensor they make."""

    def __init__(self, name, description):
        self.name = name
        self.description = description
        self.tensors = []
        self.shapes = []
        self.buffers = [None]
        self.codes = []
        self.operators = []
        self.inputs = []

    def _tensor(self, shape, dtype, name, data=None):
        tensor = schema.TensorT()
        tensor.shape = [int(size) for size in shape]
        tensor.type = {'float32': schema.TensorType.FLOAT32,
            'int32': schema.TensorType.INT32}[dtype]
        tensor.name = name or 't%d' % len(self.tensors)
        tensor.buffer = 0
        if data is not None:
            tensor.buffer = len(self.buffers)
            self.buffers.append(np.ascontiguousarray(data))
        self.tensors.append(tensor)
        self.shapes.append(tuple(int(size) for size in shape))
        return len(self.tensors) - 1

    def input(self, shape, name):
        index = self._tensor(shape, 'float32', name)
        self.inputs.append(index)
        return index

    def const(self, array, name=None):
        array = np.asarray(array)
        return self._tensor(array.shape, str(array.dtype), name, array)

    def op(self, code, inputs, shape, options=None, name=None):
        if code not in self.codes:
            self.codes.append(code)
        output = self._tensor(shape, 'float32', name)
        operator = schema.OperatorT()
        operator.opcodeIndex = self.codes.index(code)
        operator.inputs = [int(index) for index in inputs]
        operator.outputs = [output]
        if options is not None:
            kind = type(options).__name__[:-1]
            operator.builtinOptionsType = getattr(schema.BuiltinOptions,
                kind)
            operator.builtinOptions = options
        self.operators.append(operator)
        return output

    # the operators the networks are made of

    def pad(self, x, top, left, bottom, right):
        n, h, w, c = self.shapes[x]
        pads = self.const(np.int32([[0, 0], [top, bottom], [left, right],
            [0, 0]]))
        return self.op(Op.PAD, [x, pads],
            (n, h + top + bottom, w + left + right, c), schema.PadOptionsT())

    def conv(self, x, weights, bias, strides, pads, group, relu, name=None):
        """weights as ONNX has them: [out, in / group, kh, kw]."""
        out_channels, _, kh, kw = weights.shape
        n, h, w, c = self.shapes[x]
        top, left, bottom, right = pads
        same = True
        for size, k, stride, before, after in ((h, kh, strides[0], top,
                bottom), (w, kw, strides[1], left, right)):
            total = max((-(-size // stride) - 1) * stride + k - size, 0)
            if (before, after) != (total // 2, total - total // 2):
                same = False
        if not same and any(pads):
            x = self.pad(x, top, left, bottom, right)
            n, h, w, c = self.shapes[x]
            top = left = bottom = right = 0
        if same:
            oh, ow = -(-h // strides[0]), -(-w // strides[1])
        else:
            oh = (h - kh) // strides[0] + 1
            ow = (w - kw) // strides[1] + 1
        if bias is None:
            bias = np.zeros(out_channels, np.float32)
        activation = (schema.ActivationFunctionType.RELU if relu
            else schema.ActivationFunctionType.NONE)
        padding = schema.Padding.SAME if same else schema.Padding.VALID
        if group == 1:
            options = schema.Conv2DOptionsT()
            code = Op.CONV_2D
            data = weights.transpose(0, 2, 3, 1)
        elif group == c and weights.shape[1] == 1 and out_channels == c:
            options = schema.DepthwiseConv2DOptionsT()
            options.depthMultiplier = 1
            code = Op.DEPTHWISE_CONV_2D
            data = weights.transpose(1, 2, 3, 0)
        else:
            raise ValueError('grouped convolution')
        options.padding = padding
        options.strideH, options.strideW = strides
        options.dilationHFactor = options.dilationWFactor = 1
        options.fusedActivationFunction = activation
        return self.op(code, [x, self.const(data.astype(np.float32)),
            self.const(bias.astype(np.float32))], (n, oh, ow, out_channels),
            options, name)

    def binary(self, code, a, b, relu=False, name=None):
        options = {Op.ADD: schema.AddOptionsT,
            Op.MUL: schema.MulOptionsT}[code]()
        if relu:
            options.fusedActivationFunction = \
                schema.ActivationFunctionType.RELU
        shape = self.shapes[a] if len(self.shapes[a]) >= len(self.shapes[b]) \
            else self.shapes[b]
        return self.op(code, [a, b], shape, options, name)

    def prelu(self, x, slope, native, name=None):
        channels = self.shapes[x][-1]
        slope = slope.reshape(channels).astype(np.float32)
        if native:
            return self.op(Op.PRELU, [x, self.const(slope.reshape(1, 1,
                channels))], self.shapes[x], None, name)
        if slope.max() >= 1:
            # the general form: relu(x) - a * relu(-x)
            negative = self.binary(Op.MUL, x,
                self.const(np.full(channels, -1, np.float32)), relu=True)
            negative = self.binary(Op.MUL, negative, self.const(-slope))
            positive = self.op(Op.RELU, [x], self.shapes[x])
            return self.binary(Op.ADD, positive, negative, name=name)
        positive = self.binary(Op.MUL, x, self.const(1 - slope), relu=True)
        negative = self.binary(Op.MUL, x, self.const(slope))
        return self.binary(Op.ADD, positive, negative, name=name)

    def reshape(self, x, shape, name=None):
        options = schema.ReshapeOptionsT()
        options.newShape = [int(size) for size in shape]
        return self.op(Op.RESHAPE, [x, self.const(np.int32(shape))], shape,
            options, name)

    def concat(self, tensors, name=None):
        options = schema.ConcatenationOptionsT()
        options.axis = len(self.shapes[tensors[0]]) - 1
        shape = list(self.shapes[tensors[0]])
        shape[-1] = sum(self.shapes[t][-1] for t in tensors)
        return self.op(Op.CONCATENATION, tensors, shape, options, name)

    def upsample(self, x, native, name=None):
        """Every value twice in both directions (nearest neighbour)."""
        n, h, w, c = self.shapes[x]
        if native:
            options = schema.ResizeNearestNeighborOptionsT()
            return self.op(Op.RESIZE_NEAREST_NEIGHBOR, [x,
                self.const(np.int32([2 * h, 2 * w]))], (n, 2 * h, 2 * w, c),
                options, name)
        # [.., w, c] twice along the channels is [.., w, 2, c] in memory
        wide = self.reshape(self.concat([x, x]), (n, h, 1, 2 * w * c))
        # and a whole row twice is two rows
        return self.reshape(self.concat([wide, wide]), (n, 2 * h, 2 * w, c),
            name)

    def save(self, path, outputs):
        model = schema.ModelT()
        model.version = 3
        model.description = self.description
        model.operatorCodes = []
        for code in self.codes:
            operator_code = schema.OperatorCodeT()
            operator_code.deprecatedBuiltinCode = min(code, 127)
            operator_code.builtinCode = code
            operator_code.version = 1
            model.operatorCodes.append(operator_code)

        subgraph = schema.SubGraphT()
        subgraph.name = self.name
        subgraph.tensors = self.tensors
        subgraph.operators = self.operators
        subgraph.inputs = self.inputs
        subgraph.outputs = [int(index) for index in outputs]
        model.subgraphs = [subgraph]

        model.buffers = []
        size = 0
        for data in self.buffers:
            buffer = schema.BufferT()
            if data is not None:
                buffer.data = np.frombuffer(data.tobytes(), dtype=np.uint8)
                size += buffer.data.size
            model.buffers.append(buffer)

        builder = flatbuffers.Builder(size + (1 << 20))
        builder.Finish(model.Pack(builder), b'TFL3')
        with open(path, 'wb') as file:
            file.write(builder.Output())
        print('%s: %d bytes, %d operators' % (path, os.path.getsize(path),
            len(self.operators)))


# -- ONNX to TensorFlow Lite -------------------------------------------------

def attributes(node):
    return {a.name: onnx.helper.get_attribute_value(a)
        for a in node.attribute}


def bn_terms(constants, node):
    """scale and shift of a BatchNormalization: y = x * scale + shift."""
    gamma, beta, mean, variance = (constants[name]
        for name in node.input[1:5])
    epsilon = attributes(node).get('epsilon', 1e-5)
    scale = gamma / np.sqrt(variance + epsilon)
    return scale, beta - mean * scale


def convert(path, graph, input_shape=None, native=False, swap_input=False,
        skip_input_arithmetic=False):
    """Rewrites the ONNX file at \a path into \a graph; returns the outputs
    (tensor numbers, in the ONNX file's order)."""
    model = onnx.load(path)
    constants = {i.name: numpy_helper.to_array(i)
        for i in model.graph.initializer}
    nodes = list(model.graph.node)
    consumers = {}
    for node in nodes:
        for name in node.input:
            consumers.setdefault(name, []).append(node)

    def only_consumer(name, kind):
        users = consumers.get(name, [])
        if len(users) == 1 and users[0].op_type == kind:
            return users[0]
        return None

    source = [i for i in model.graph.input if i.name not in constants][0]
    dims = [d.dim_value for d in source.type.tensor_type.shape.dim]
    if input_shape is not None:
        dims = [1, 3, input_shape[0], input_shape[1]]
    values = {source.name: graph.input((1, dims[2], dims[3], 3), 'input')}
    # a per-channel scale and shift that waits for the layer after it
    pending = {}
    done = set()
    first_conv = True

    for node in nodes:
        if id(node) in done:
            continue
        kind = node.op_type
        a = attributes(node)
        x = values.get(node.input[0]) if node.input else None
        out = node.output[0]

        if kind in ('Sub', 'Mul') and skip_input_arithmetic \
                and node.input[0] in (source.name, '_minusscalar0'):
            print('  input arithmetic left to the caller: %s %g'
                % (kind, float(constants[node.input[1]].ravel()[0])))
            values[out] = x
        elif kind == 'Conv':
            weights = constants[node.input[1]].astype(np.float64)
            bias = constants[node.input[2]].astype(np.float64) \
                if len(node.input) > 2 else np.zeros(weights.shape[0])
            if first_conv and swap_input:
                weights = weights[:, ::-1]
            first_conv = False
            last = node
            bn = only_consumer(out, 'BatchNormalization')
            if bn is not None:
                scale, shift = bn_terms(constants, bn)
                weights = weights * scale[:, None, None, None]
                bias = bias * scale + shift
                done.add(id(bn))
                last = bn
            relu = only_consumer(last.output[0], 'Relu')
            if relu is not None:
                done.add(id(relu))
                last = relu
            values[last.output[0]] = graph.conv(x, weights, bias,
                a.get('strides', [1, 1]), a.get('pads', [0, 0, 0, 0]),
                a.get('group', 1), relu is not None)
            if a.get('dilations', [1, 1]) != [1, 1]:
                raise ValueError('dilated convolution')
        elif kind == 'Relu':
            values[out] = graph.op(Op.RELU, [x], graph.shapes[x])
        elif kind == 'PRelu':
            values[out] = graph.prelu(x, constants[node.input[1]], native)
        elif kind == 'MaxPool':
            if a.get('pads', [0, 0, 0, 0]) != [0, 0, 0, 0] \
                    or a.get('ceil_mode', 0):
                raise ValueError('padded pooling')
            options = schema.Pool2DOptionsT()
            options.padding = schema.Padding.VALID
            options.filterHeight, options.filterWidth = a['kernel_shape']
            options.strideH, options.strideW = a['strides']
            n, h, w, c = graph.shapes[x]
            values[out] = graph.op(Op.MAX_POOL_2D, [x], (n,
                (h - options.filterHeight) // options.strideH + 1,
                (w - options.filterWidth) // options.strideW + 1, c),
                options)
        elif kind == 'Resize':
            scales = constants[node.input[2]]
            if a.get('mode', b'nearest') != b'nearest' \
                    or list(scales) != [1, 1, 2, 2]:
                raise ValueError('a Resize that is not nearest by two')
            values[out] = graph.upsample(x, native)
        elif kind == 'Add':
            values[out] = graph.binary(Op.ADD, x, values[node.input[1]])
        elif kind == 'Sigmoid':
            values[out] = graph.op(Op.LOGISTIC, [x], graph.shapes[x])
        elif kind == 'Transpose':
            if a['perm'] != [0, 2, 3, 1]:
                raise ValueError('a Transpose that is not NCHW to NHWC')
            values[out] = x		# it is NHWC already
        elif kind == 'Reshape':
            n, h, w, c = graph.shapes[x]
            values[out] = graph.reshape(x, (n, h * w, c), out)
        elif kind == 'BatchNormalization':
            pending[out] = bn_terms(constants, node)
            values[out] = x
        elif kind in ('Dropout', 'Flatten'):
            if node.input[0] in pending:
                pending[out] = pending[node.input[0]]
            values[out] = x
        elif kind == 'Gemm':
            # ONNX flattens [c, h, w]; the tensor here is [h, w, c]
            n, h, w, c = graph.shapes[x]
            weights = constants[node.input[1]].astype(np.float64)
            bias = constants[node.input[2]].astype(np.float64)
            units = weights.shape[0]
            weights = weights.reshape(units, c, h, w)
            if node.input[0] in pending:
                scale, shift = pending[node.input[0]]
                bias = bias + (weights
                    * shift[None, :, None, None]).sum(axis=(1, 2, 3))
                weights = weights * scale[None, :, None, None]
            last = node
            bn = only_consumer(out, 'BatchNormalization')
            if bn is not None:
                scale, shift = bn_terms(constants, bn)
                weights = weights * scale[:, None, None, None]
                bias = bias * scale + shift
                done.add(id(bn))
                last = bn
            weights = weights.transpose(0, 2, 3, 1).reshape(units, h * w * c)
            flat = graph.reshape(x, (n, h * w * c))
            options = schema.FullyConnectedOptionsT()
            values[last.output[0]] = graph.op(Op.FULLY_CONNECTED, [flat,
                graph.const(weights.astype(np.float32)),
                graph.const(bias.astype(np.float32))], (n, units), options,
                last.output[0])
        else:
            raise ValueError('operator %s' % kind)

    # the outputs keep their names: that is how they are found
    for output in model.graph.output:
        graph.tensors[values[output.name]].name = output.name
    return [values[o.name] for o in model.graph.output]


# -- checking ---------------------------------------------------------------

def run_tflite(path, data):
    from ai_edge_litert.interpreter import Interpreter
    interpreter = Interpreter(model_path=path)
    interpreter.allocate_tensors()
    interpreter.set_tensor(interpreter.get_input_details()[0]['index'], data)
    interpreter.invoke()
    return [interpreter.get_tensor(d['index'])
        for d in interpreter.get_output_details()]


def check(onnx_path, tflite_path, to_onnx):
    """Both files on the same random picture; \a to_onnx makes ONNX's input
    of the TensorFlow Lite one."""
    import onnxruntime
    from ai_edge_litert.interpreter import Interpreter
    interpreter = Interpreter(model_path=tflite_path)
    shape = interpreter.get_input_details()[0]['shape']
    rng = np.random.default_rng(1)
    # something like a picture: smooth, with texture
    small = rng.uniform(0, 255, (1, shape[1] // 8 + 1, shape[2] // 8 + 1, 3))
    picture = np.kron(small, np.ones((1, 8, 8, 1)))[:, :shape[1], :shape[2]]
    picture = (picture + rng.uniform(-20, 20, picture.shape)).clip(0, 255) \
        .astype(np.float32)
    lite_input, onnx_input = to_onnx(picture)
    session = onnxruntime.InferenceSession(onnx_path,
        providers=['CPUExecutionProvider'])
    reference = session.run(None, {session.get_inputs()[0].name: onnx_input})
    ours = run_tflite(tflite_path, lite_input)
    worst = 0
    for theirs, mine in zip(reference, ours):
        if theirs.shape != mine.shape:
            sys.exit('%s: shape %s, ONNX has %s' % (tflite_path, mine.shape,
                theirs.shape))
        worst = max(worst, float(np.abs(theirs - mine).max()
            / max(np.abs(theirs).max(), 1e-6)))
    print('  against ONNX Runtime: largest relative difference %.2g' % worst)
    if worst > 1e-3:
        sys.exit('%s does not agree with %s' % (tflite_path, onnx_path))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    parser.add_argument('--work', default=os.path.join(here, 'download'))
    parser.add_argument('--out',
        default=os.path.join(here, '..', '..', 'models'))
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--native', action='store_true',
        help='keep PRELU and RESIZE_NEAREST_NEIGHBOR (CPU only; for '
            'comparing)')
    parser.add_argument('--sizes', default='640,1280',
        help='detector input sizes')
    args = parser.parse_args()
    os.makedirs(args.work, exist_ok=True)
    os.makedirs(args.out, exist_ok=True)
    suffix = '_native' if args.native else ''

    yunet = fetch('face_detection_yunet_2023mar.onnx', args.work)
    for size in [int(s) for s in args.sizes.split(',')]:
        graph = Graph('face_detector', 'YuNet 2023mar (opencv_zoo, MIT) for '
            'the air/OS Neural Kit: BGR 0..255 in, cls/obj/bbox/kps per '
            'stride out')
        outputs = convert(yunet, graph, (size, size), native=args.native)
        path = os.path.join(args.out, 'face_detector_%d%s.tflite'
            % (size, suffix))
        graph.save(path, outputs)
        if args.check and size == 640:
            check(yunet, path, lambda p: (p, p.transpose(0, 3, 1, 2)))

    sface = fetch('face_recognition_sface_2021dec.onnx', args.work)
    graph = Graph('face_embedder', 'SFace 2021dec (opencv_zoo, Apache-2.0) '
        'for the air/OS Neural Kit: (BGR - 127.5) / 128 in, 128 numbers out')
    outputs = convert(sface, graph, native=args.native, swap_input=True,
        skip_input_arithmetic=True)
    path = os.path.join(args.out, 'face_embedder%s.tflite' % suffix)
    graph.save(path, outputs)
    if args.check:
        # ONNX takes RGB 0..255 and does the arithmetic itself
        check(sface, path, lambda p: ((p - 127.5) / 128,
            p[..., ::-1].transpose(0, 3, 1, 2).copy()))


if __name__ == '__main__':
    main()
