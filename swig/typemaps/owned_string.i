/* Opt-in typemap for malloc/calloc-owned char ** string output. */
%typemap(in,numinputs=0) char **DEFW_OWNED_STRING (char *tmp) %{
    tmp = NULL;
    $1 = &tmp;
%}

%typemap(argout) char **DEFW_OWNED_STRING (PyObject *obj) %{
    if (*$1 == NULL) {
        PyErr_NoMemory();
        SWIG_fail;
    }
    obj = PyUnicode_FromString(*$1);
#if SWIG_VERSION >= 0x040100
    $result = SWIG_Python_AppendOutput($result, obj, $isvoid);
#else
    $result = SWIG_Python_AppendOutput($result, obj);
#endif
%}

%typemap(freearg) char **DEFW_OWNED_STRING %{
    if (*$1)
       free(*$1);
%}
