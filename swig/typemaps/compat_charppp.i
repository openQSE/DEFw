/* Compatibility typemap for current DEFw char *** output parameters. */
%typemap(in, numinputs=0) char *** (char **temp) {
        temp = NULL;
        $1 = &temp;
}
%typemap(argout) char *** {
        PyObject *o, *o2, *o3;
        o = SWIG_NewPointerObj(SWIG_as_voidptr(*$1), $*1_descriptor,
                               SWIG_POINTER_OWN);
        if ((!$result) || ($result == Py_None))
                $result = o;
        else
        {
                if(!PyTuple_Check($result))
                {
                        o2 = $result;
                        $result = PyTuple_New(1);
                        PyTuple_SetItem($result, 0, o2);
                }
                o3 = PyTuple_New(1);
                PyTuple_SetItem(o3, 0, o);
                o2 = $result;
                $result = PySequence_Concat(o2, o3);
                Py_DECREF(o2);
                Py_DECREF(o3);
        }
}
