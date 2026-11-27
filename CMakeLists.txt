cmake_minimum_required(VERSION 3.20)

project(DEFw
	VERSION 0.1.0
	LANGUAGES C)

include(CTest)
include(GNUInstallDirs)

# DEFw: the C runtime over Mercury and Margo, its directory service, the
# typed QPM APIs and the Python binding. See src2/CMakeLists.txt.
add_subdirectory(src2)

# The Python modules that defw2.compat runs QFw's Python with. It loads them
# from <prefix>/python. Python that runs outside defw2-python imports the
# infra modules from site-packages, where the defw2 package is.
install(DIRECTORY python/
	DESTINATION python
	PATTERN "__pycache__" EXCLUDE)
if(DEFINED DEFW2_PYTHON_INSTALL_DIR)
	install(DIRECTORY python/infra/
		DESTINATION "${DEFW2_PYTHON_INSTALL_DIR}"
		FILES_MATCHING PATTERN "*.py")
endif()
