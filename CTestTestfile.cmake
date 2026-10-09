# CMake generated Testfile for 
# Source directory: /home/swana/dev/search/zepra
# Build directory: /home/swana/dev/search/zepra
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(ZepraBasicTest "F:/ketivee_org_project/zepra/bin/Debug/zepra_test.exe")
  set_tests_properties(ZepraBasicTest PROPERTIES  _BACKTRACE_TRIPLES "F:/ketivee_org_project/zepra/CMakeLists.txt;180;add_test;F:/ketivee_org_project/zepra/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(ZepraBasicTest "F:/ketivee_org_project/zepra/bin/Release/zepra_test.exe")
  set_tests_properties(ZepraBasicTest PROPERTIES  _BACKTRACE_TRIPLES "F:/ketivee_org_project/zepra/CMakeLists.txt;180;add_test;F:/ketivee_org_project/zepra/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(ZepraBasicTest "F:/ketivee_org_project/zepra/bin/MinSizeRel/zepra_test.exe")
  set_tests_properties(ZepraBasicTest PROPERTIES  _BACKTRACE_TRIPLES "F:/ketivee_org_project/zepra/CMakeLists.txt;180;add_test;F:/ketivee_org_project/zepra/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(ZepraBasicTest "F:/ketivee_org_project/zepra/bin/RelWithDebInfo/zepra_test.exe")
  set_tests_properties(ZepraBasicTest PROPERTIES  _BACKTRACE_TRIPLES "F:/ketivee_org_project/zepra/CMakeLists.txt;180;add_test;F:/ketivee_org_project/zepra/CMakeLists.txt;0;")
else()
  add_test(ZepraBasicTest NOT_AVAILABLE)
endif()
