cmake_minimum_required (VERSION 3.1)
project (kms_message C)

set (CMAKE_C_STANDARD 90)

add_library (
   kms_message SHARED
   src/kms_kv_list.c
   src/kms_kv_list.h
   src/kms_message.h
   src/kms_request.c
   src/kms_request.h
   src/kms_request_str.c
   src/kms_request_str.h
)

add_executable (
   test_kms_request
   test/test_kms.c
   test/test_kms.h
   test/test_kms_request.c
)

target_link_libraries (test_kms_request kms_message)
target_include_directories (test_kms_request PRIVATE ${PROJECT_SOURCE_DIR})