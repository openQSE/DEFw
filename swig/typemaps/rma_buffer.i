/* Map an RMA input buffer and its size from one Python bytes object. */
%typemap(in) (const void *rma_src, size_t rma_srclen) %{
	char *rma_in_buf = NULL;
	Py_ssize_t rma_in_len = 0;

	if (PyBytes_AsStringAndSize($input, &rma_in_buf, &rma_in_len) < 0)
		SWIG_fail;
	$1 = rma_in_buf;
	$2 = (size_t)rma_in_len;
%}

/* Return a malloc-owned RMA output buffer and size as Python bytes. */
%typemap(in, numinputs=0) (char **rma_buf, size_t *rma_len)
	(char *rma_out_buf, size_t rma_out_len) %{
	rma_out_buf = NULL;
	rma_out_len = 0;
	$1 = &rma_out_buf;
	$2 = &rma_out_len;
%}

%typemap(argout) (char **rma_buf, size_t *rma_len) (PyObject *obj) %{
	if (*$1) {
		obj = PyBytes_FromStringAndSize(*$1, (Py_ssize_t)*$2);
	} else {
		obj = Py_None;
		Py_INCREF(obj);
	}
#if SWIG_VERSION >= 0x040100
	$result = SWIG_Python_AppendOutput($result, obj, $isvoid);
#else
	$result = SWIG_Python_AppendOutput($result, obj);
#endif
%}

%typemap(freearg) (char **rma_buf, size_t *rma_len) %{
	if (*$1)
		free(*$1);
%}
