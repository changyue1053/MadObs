find_package(JNI REQUIRED)
find_package(Java REQUIRED COMPONENTS Development Runtime)
add_library(madobs_jni SHARED private_jni.cc)
target_include_directories(madobs_jni PRIVATE ${JNI_INCLUDE_DIRS})
target_link_libraries(madobs_jni PRIVATE MadObs::runtime)
target_compile_features(madobs_jni PRIVATE cxx_std_17)
target_compile_options(madobs_jni PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(imobs_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(madobs_jni PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(UNIX AND NOT APPLE)
  target_link_options(madobs_jni PRIVATE "-Wl,--exclude-libs,ALL" "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/jni.exports")
  set_property(TARGET madobs_jni APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/jni.exports")
endif()
set(java_classes "${CMAKE_CURRENT_BINARY_DIR}/classes")
add_custom_command(OUTPUT "${java_classes}/org/madobs/PrivateRuntime.class"
  BYPRODUCTS "${java_classes}/org_madobs_PrivateRuntime.h" "${java_classes}/org/madobs/PrivateRuntime$Exporter.class"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${java_classes}"
  COMMAND ${Java_JAVAC_EXECUTABLE} -Xlint:all -Werror -h "${java_classes}" -d "${java_classes}"
    "${CMAKE_CURRENT_SOURCE_DIR}/java/org/madobs/PrivateRuntime.java"
  DEPENDS java/org/madobs/PrivateRuntime.java)
add_custom_target(madobs_java ALL DEPENDS "${java_classes}/org/madobs/PrivateRuntime.class")
add_dependencies(madobs_jni madobs_java)
target_include_directories(madobs_jni PRIVATE "${java_classes}")
if(BUILD_TESTING)
  add_custom_command(OUTPUT "${java_classes}/JvmSmoke.class"
    COMMAND ${Java_JAVAC_EXECUTABLE} -Xlint:all -Werror -cp "${java_classes}" -d "${java_classes}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/JvmSmoke.java"
    DEPENDS tests/JvmSmoke.java "${java_classes}/org/madobs/PrivateRuntime.class")
  add_custom_target(madobs_java_tests ALL DEPENDS "${java_classes}/JvmSmoke.class")
  add_dependencies(madobs_java_tests madobs_java)
  add_executable(test_madobs_jni_failure tests/jni_failure.cc)
  target_include_directories(test_madobs_jni_failure PRIVATE ${JNI_INCLUDE_DIRS} "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_features(test_madobs_jni_failure PRIVATE cxx_std_11)
  target_compile_options(test_madobs_jni_failure PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME madobs_jni_failure COMMAND test_madobs_jni_failure)
  add_executable(test_madobs_jni_send_failure tests/jni_send_failure.cc)
  add_dependencies(test_madobs_jni_send_failure madobs_java)
  target_include_directories(test_madobs_jni_send_failure PRIVATE ${JNI_INCLUDE_DIRS} "${java_classes}")
  target_compile_features(test_madobs_jni_send_failure PRIVATE cxx_std_17)
  target_compile_options(test_madobs_jni_send_failure PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(test_madobs_jni_send_failure PRIVATE MadObs::runtime)
  add_test(NAME madobs_jni_send_failure COMMAND test_madobs_jni_send_failure)
  add_test(NAME madobs_jvm COMMAND ${Java_JAVA_EXECUTABLE} -Xcheck:jni
    "-Djava.library.path=$<TARGET_FILE_DIR:madobs_jni>" -cp "${java_classes}" JvmSmoke)
  set_tests_properties(madobs_jvm PROPERTIES TIMEOUT 60)
endif()

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/madobs-jvm.jar"
  COMMAND ${Java_JAR_EXECUTABLE} --create --file "${CMAKE_CURRENT_BINARY_DIR}/madobs-jvm.jar" -C "${java_classes}" org/madobs
  DEPENDS "${java_classes}/org/madobs/PrivateRuntime.class")
add_custom_target(madobs_java_jar ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/madobs-jvm.jar")
add_dependencies(madobs_java_jar madobs_java)
install(TARGETS madobs_jni LIBRARY DESTINATION lib)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/madobs-jvm.jar" DESTINATION share/MadObs/java)
