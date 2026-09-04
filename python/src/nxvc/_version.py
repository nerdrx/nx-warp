"""Version of the ``nxvc`` Python bindings.

This file is the single source of truth for the package version and is kept
deliberately import-free so that the build backend can read it statically.

There is no version macro in the C library to derive it from: ``nxvc.h``
defines ``NXVC_VERSION 1`` (that is the *bitstream* version, not the library
version) and the project version lives in the root ``CMakeLists.txt`` as
``project(nxwarp VERSION x.y.z)``.  So the package version is static here and
``python/tests/test_version.py`` asserts it against both of those whenever the
repository is checked out next to the installed package -- if the root
CMakeLists version moves and this file does not, that test fails.
"""

#: Package version.  Must equal ``project(nxwarp VERSION ...)`` in the root
#: CMakeLists.txt of the nx-warp repository.
__version__ = "0.0.1"

#: Bitstream/ABI version this binding is written against.  Must equal the
#: ``NXVC_VERSION`` macro in ``include/nxvc/nxvc.h``.
NXVC_VERSION = 1
