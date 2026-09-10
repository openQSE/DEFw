from defw_remote import defwrc


FAIL = -1


def run():
	return defwrc(
		FAIL,
		msg="intentional failure for defw_test_runner status handling",
	)
