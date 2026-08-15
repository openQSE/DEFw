#include "fixture.h"

const char *defw_external_swig_fixture_name(void)
{
	return "external-swig-fixture";
}

int defw_external_swig_fixture_add(int lhs, int rhs)
{
	return lhs + rhs;
}
