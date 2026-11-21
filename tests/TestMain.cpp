#include "Test.h"

int
main(int argc, char** argv)
{
	return test::RunAll(argc > 1 ? argv[1] : nullptr);
}
