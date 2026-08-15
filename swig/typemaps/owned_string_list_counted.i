/* Opt-in typemap for char ***out, size_t *count owned string lists. */
%typemap(in,numinputs=0) (char ***DEFW_OWNED_STRING_LIST, size_t *DEFW_OWNED_STRING_LIST_COUNT) (char **tmp, size_t tmp_count) %{
    tmp = NULL;
    tmp_count = 0;
    $1 = &tmp;
    $2 = &tmp_count;
%}

%typemap(argout) (char ***DEFW_OWNED_STRING_LIST, size_t *DEFW_OWNED_STRING_LIST_COUNT) %{
    PyObject *list = PyList_New(*$2);
    if (!list)
        SWIG_fail;
    for (size_t i = 0; i < *$2; ++i) {
        PyObject *item = PyUnicode_FromString((*$1)[i]);
        if (!item) {
            Py_DECREF(list);
            SWIG_fail;
        }
        PyList_SET_ITEM(list, i, item);
    }
#if SWIG_VERSION >= 0x040100
    $result = SWIG_Python_AppendOutput($result, list, $isvoid);
#else
    $result = SWIG_Python_AppendOutput($result, list);
#endif
%}

%typemap(freearg) (char ***DEFW_OWNED_STRING_LIST, size_t *DEFW_OWNED_STRING_LIST_COUNT) %{
    if (*$1) {
        for (size_t i = 0; i < *$2; ++i)
            free((*$1)[i]);
        free(*$1);
    }
%}
