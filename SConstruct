import os, glob, sysconfig, sys, yaml

DEFW_PATH = Dir('.').abspath
LIBDEFW_SRC_FILES = glob.glob(os.path.join(DEFW_PATH, "src", "libdefw*.c"))
DEFW_FWSL_FILES = glob.glob(os.path.join(DEFW_PATH, "src", "*.c"))
DEFW_FWSL_FILES = [entry for entry in DEFW_FWSL_FILES if entry not in LIBDEFW_SRC_FILES \
        and 'defw.c' != os.path.basename(entry) and '_wrap' not in entry]
DEFW_EXTERNAL_LIBRARIES = []

env = Environment()

def generate_include_paths():
    import subprocess
    try:
        result = subprocess.check_output(
            "gcc -xc -E -v /dev/null 2>&1 | grep '^ /' | tr -d ' '", 
              shell=True, text=True).strip()
        include_paths = result.split("\n") if result else []
    except subprocess.CalledProcessError as e:
        print("Error running command:", e)

    string = ''
    for i in include_paths:
        string += f"-I{i} "

    return string

def generate_swig_intf(source, env):
    cmd = env['PYTHON'] + " " + env['GEN_SWIG_INTF'] + " " + source
    print(cmd)
    os.system(cmd)
    return os.path.splitext(source)[0]+'.i'

def swigify(ifile, target_lib, link_libs, include_path, lib_path, env):
    cmd = env['SWIG'] + " -threads -python -includeall " + env['SWIG_FLAGS'] + " " + env['SWIG_INCLUDES'] + " " + include_path + " " + generate_include_paths() + " " + ifile
    os.system(cmd)
    wfile = os.path.splitext(ifile)[0]+"_wrap.c"
    cmd = env['CC'] + " " + env['SWIG_COMP_FLAGS'] + " -I" + env['PYTHON_INCLUDE_DIR'] + \
            " " + include_path + " " + lib_path + \
            " -shared " + "-o " + target_lib + \
            " -L" + env['LINK_PATH'] + " " + " ".join(link_libs) + \
            " -L" + env['PYTHON_LIB_DIR'] + " -l" + env['PYTHON_LIB'] + " " + wfile
    print(cmd)
    os.system(cmd)

def build_shared_library(env):
    for cfile in env['LIBDEFW_SRC_FILES']:
        so = os.path.splitext(cfile)[0]+".so"
        cmd = env['CC'] + " " + env['SWIG_COMP_FLAGS'] + " -shared -luuid -o " + so + " " + cfile
        print(cmd)
        os.system(cmd)

def mbuild_shared_library(env, files, so):
    print("building shared library from ", " ".join(files))
    cmd = env['CC'] + " " + env['SWIG_COMP_FLAGS'] + \
            " -I" + env['PYTHON_INCLUDE_DIR'] + \
            " -fPIC -shared -luuid -o " + so + " " + \
            " ".join(files)
    print(cmd)
    os.system(cmd)

def swigify_all_files(env):
    swgs = {}
    for swg in env['DEFW_SWG_FILES']:
        swgs[swg] = ["-l"+os.path.splitext(os.path.basename(swg))[0]]
        if "_agent" in swg:
            swgs[swg].append("-ldefw_connect")

    for k, v in swgs.items():
        ifile = generate_swig_intf(k, env)
        base_name = os.path.basename(ifile)
        lib_name = os.path.join(env['DEFW_PATH'], "src", "_c"+os.path.splitext(base_name)[0]+".so")
        swigify(ifile, lib_name, v, '', '', env)

def cleanup_external_files(env):
    cy = read_configuration_file(env)
    if not cy:
        return

    swigify_info = cy['defw']['swigify']
    for entry in swigify_info:
       cleanup_name = os.path.join(env['DEFW_PATH'], "src", "*"+entry['name']+"*")
       clean_up_cmd = f"rm -Rf {cleanup_name}"
       print(clean_up_cmd)
       os.system(clean_up_cmd)

def read_configuration_file(env):
    cfg = ARGUMENTS.get('CONFIG', '')
    if not cfg:
        return None
    with open(cfg, 'r') as f:
        cy = yaml.load(f, Loader=yaml.FullLoader)

    if 'defw' not in cy or 'swigify' not in cy['defw']:
        print(f"Badly formed configuration file {cfg}")
        return None

    return cy

def swigify_externals(env):
    cy = read_configuration_file(env)
    if not cy:
        return

    swigify_info = cy['defw']['swigify']
    for entry in swigify_info:
       path = entry['path']
       if 'files' in entry:
            files = entry['files']
       else:
            files = glob.glob(os.path.join(path, "*.h"))
       swg_name = os.path.join(env['DEFW_PATH'], "src", entry['name']+".swg")
       with open(swg_name, 'w') as f:
           f.write(f"%module c{entry['name']}\n")
           f.write('%include "cwstring.i"\n')
           f.write('%rename("%(strip:[__])s", regexmatch$name="__.*") "";\n')
           f.write("%{\n")
           if 'addendums' in entry:
               for addendum in entry['addendums']:
                   with open(addendum, 'r') as a:
                       f.write(a.read())
           for file in files:
               f.write(f'#include "{file}"\n')
           f.write("%}\n")
           if 'typemaps' in entry:
               for typemap in entry['typemaps']:
                   with open(typemap, 'r') as t:
                       f.write(t.read())
           f.write("typedef long long ssize_t;\n")
           f.write("typedef unsigned long long uint64_t;\n")
           f.write("typedef unsigned int uint32_t;\n")
           f.write("typedef unsigned short uint16_t;\n")
           f.write("typedef unsigned char uint8_t;\n")
           for ignore in entry['ignore']:
               f.write(f"%ignore {ignore};\n")
           for file in files:
               f.write(f'%include "{file}"\n')
       import subprocess
       try:
           result = subprocess.check_output(["pkg-config", "--variable=prefix",
                                          entry['name']], text=True).strip()
       except subprocess.CalledProcessError as e:
           print("Error running pkg-config:", e)

       include_path = "-I"+os.path.join(result, 'include')
       lib_path = "-L"+os.path.join(result, 'lib')

       ifile = generate_swig_intf(swg_name, env)
       base_name = os.path.basename(ifile)
       lib_name = os.path.join(env['DEFW_PATH'], "src", "_c"+os.path.splitext(base_name)[0]+".so")
       libs = []
       for lib in entry['libs']:
           libs.append(f"-l{lib}")
       swigify(ifile, lib_name, libs, include_path, lib_path, env)

def install(env, src, dst):
    #cmd = "cp " + os.path.join(src, "*.so") + " " + dst
    #print(cmd)
    #os.system(cmd)
    #cmd = "cp " + os.path.join(src, "*.py") + " " + dst
    #print(cmd)
    #os.system(cmd)
    return

def build_bin(env):
    binary = env['DEFW_MAIN_C']
    path = os.path.join(env['DEFW_PATH'], "src", "defwp")
    print("Building standard bin")

    cmd = env['CC'] + " " + env['SWIG_COMP_FLAGS'] + \
            " -I" + env['PYTHON_INCLUDE_DIR'] + " " + \
            binary + " -L" + env['LINK_PATH'] + " -L" + env['PYTHON_LIB_DIR'] + \
            " -lfwsl -ldefw_global -ldefw_connect -ldefw_agent -luuid -l" + env['PYTHON_LIB'] + \
            " -o " + path
    print(cmd)
    os.system(cmd)

def clean(env):
    pfiles = os.path.join(env['DEFW_PATH'], "src", "*.py")
    wfiles = os.path.join(env['DEFW_PATH'], "src", "*_wrap.c")
    ofiles = os.path.join(env['DEFW_PATH'], "src", "*.o")
    ifiles = os.path.join(env['DEFW_PATH'], "src", "*.i")
    cmd = "rm -Rf " + " ".join([pfiles, wfiles, ofiles, ifiles])
    print(cmd)
    os.system(cmd)
    cleanup_external_files(env)

print(sys.version)

env['PYTHON'] = "python3"
env['SWIG'] = "swig"
env['SWIG_COMP_FLAGS'] = "-g -Wall -fPIC"
env['SWIG_FLAGS'] = "-D__x86_64__ -D__arch_lib__ -D_LARGEFILE64_SOURCE=1"
env['SWIG_INCLUDES'] = ""
#env['SWIG_INCLUDES'] = "-I/usr/include/c++/13/tr1/ -I/usr/include -I/usr/include/linux -I/usr/include/x86_64-linux-gnu/"
env['GEN_SWIG_INTF'] = os.path.join('swig_templates', 'generate_swig_i.py')
env['DEFW_PATH'] = DEFW_PATH
env['DEFW_SWG_FILES'] = glob.glob(os.path.join(env['DEFW_PATH'], "src", "*.swg"))
env['LIBDEFW_SRC_FILES'] = LIBDEFW_SRC_FILES
env['DEFW_FWSL_FILES'] = DEFW_FWSL_FILES
env['DEFW_HDR_FILES'] = glob.glob(os.path.join(env['DEFW_PATH'], "src", "*.h"))
env['LINK_PATH'] = os.path.join(env['DEFW_PATH'], "src")
env['PYTHON_INCLUDE_DIR'] = sysconfig.get_config_var('INCLUDEPY')
env['PYTHON_LIB_DIR'] = sysconfig.get_config_var('LIBDIR')
env['PYTHON_LIB'] = os.path.splitext(sysconfig.get_config_var('LDLIBRARY').strip('lib'))[0]
env['DEFW_MAIN_C'] = os.path.join(DEFW_PATH, 'src', 'defw.c')

env.AddMethod(generate_swig_intf)
env.AddMethod(swigify)
env.AddMethod(swigify_all_files)
env.AddMethod(swigify_externals)
env.AddMethod(build_shared_library)
env.AddMethod(mbuild_shared_library)
env.AddMethod(install)
env.AddMethod(build_bin)
env.AddMethod(clean)

env.clean_all = env.clean()
env.shared_libs = env.build_shared_library()
env.swigify_files = env.swigify_all_files()
env.swigify_externals = env.swigify_externals()
env.fwsl = env.mbuild_shared_library(env['DEFW_FWSL_FILES'], os.path.join(env['DEFW_PATH'], "src", "libfwsl.so"))
env.bin = env.build_bin()
env.install_defw = env.install(os.path.join(env['DEFW_PATH'], "src"),
                              os.path.join(env['DEFW_PATH'], "install"))

