/* Opt-in typed opaque handle helper for future DEFw handle wrappers. */
%define DEFW_OPAQUE_HANDLE(TYPE)
%typemap(out) TYPE * {
    $result = SWIG_NewPointerObj(SWIG_as_voidptr($1), $1_descriptor, 0);
}
%enddef
