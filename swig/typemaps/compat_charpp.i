/* Compatibility typemap for current DEFw char ** output parameters. */
%typemap(in,numinputs=0) char** (char* tmp) %{
    tmp = NULL;
    $1 = &tmp;
%}

%typemap(argout) char** (PyObject* obj) %{
    if (*$1 == NULL)
       goto fail;
    obj = PyUnicode_FromString(*$1);
#if SWIG_VERSION >= 0x040100
    $result = SWIG_Python_AppendOutput($result, obj, $isvoid);
#else
    $result = SWIG_Python_AppendOutput($result, obj);
#endif
%}

%typemap(freearg) char** %{
    if (*$1)
       free(*$1);
%}
