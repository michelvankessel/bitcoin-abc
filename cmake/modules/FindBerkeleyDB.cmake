# Try to find the BerkeleyDB librairies
# BDB_FOUND - system has Berkeley DB lib
# BDB_INCLUDE_DIR - the Berkeley DB include directory
# BDB_LIBRARY - Library needed to use Berkeley DB
# BDBXX_INCLUDE_DIR - the Berkeley DB include directory for C++
# BDBXX_LIBRARY - Library needed to use Berkeley DB C++ API

find_path(BDB_INCLUDE_DIR NAMES db.h)
find_library(BDB_LIBRARY NAMES db libdb)

find_path(BDBXX_INCLUDE_DIR NAMES db_cxx.h)
find_library(BDBXX_LIBRARY NAMES db_cxx libdb_cxx)

MESSAGE(STATUS "BerkeleyDB libs: " ${BDB_LIBRARY} " " ${BDBXX_LIBRARY})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(BerkeleyDB DEFAULT_MSG BDB_INCLUDE_DIR BDB_LIBRARY BDBXX_INCLUDE_DIR BDBXX_LIBRARY)

mark_as_advanced(BDB_INCLUDE_DIR BDB_LIBRARY BDBXX_INCLUDE_DIR BDBXX_LIBRARY)

set(BerkeleyDB_LIBRARIES ${BDB_LIBRARY} ${BDBXX_LIBRARY})
set(BerkeleyDB_INCLUDE_DIRS ${BDB_INCLUDE_DIR} ${BDBXX_INCLUDE_DIR})
