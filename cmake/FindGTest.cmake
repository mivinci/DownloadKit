# FindGTest.cmake - 检查并配置本地的 Google Test
#
# 查找结果变量：
#   GTest_FOUND          - 是否找到 GTest
#   GTest_INCLUDE_DIRS   - GTest 头文件目录
#   GTest_LIBRARIES      - GTest 库文件
#   GTest_MAIN_LIBRARIES - GTest main 库文件
#
# 导入目标：
#   GTest::gtest         - GTest 库目标
#   GTest::gtest_main    - GTest main 库目标

# 防止重复查找
if(GTest_FOUND)
  return()
endif()

# ---- 方式 1：尝试 CMake Config 模式 ----
find_package(GTest CONFIG QUIET)
if(GTest_FOUND)
  message(STATUS "FindGTest: 通过 CMake Config 模式找到 GTest")
  return()
endif()

# ---- 方式 2：尝试 pkg-config ----
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(_GTEST QUIET gtest)
  if(_GTEST_FOUND)
    set(GTest_INCLUDE_DIRS ${_GTEST_INCLUDE_DIRS})
    set(GTest_LIBRARIES    ${_GTEST_LIBRARIES})

    pkg_check_modules(_GTEST_MAIN QUIET gtest_main)
    if(_GTEST_MAIN_FOUND)
      set(GTest_MAIN_LIBRARIES ${_GTEST_MAIN_LIBRARIES})
    endif()
  endif()
endif()

# ---- 方式 3：手动搜索常见路径 ----
if(NOT GTest_INCLUDE_DIRS)
  find_path(GTest_INCLUDE_DIRS
    NAMES gtest/gtest.h
    PATHS
      /usr/local/include
      /usr/include
      /opt/homebrew/include
      $ENV{GTEST_ROOT}/include
    NO_DEFAULT_PATH
  )
  # 回退到默认搜索路径
  if(NOT GTest_INCLUDE_DIRS)
    find_path(GTest_INCLUDE_DIRS NAMES gtest/gtest.h)
  endif()
endif()

if(NOT GTest_LIBRARIES)
  find_library(GTest_LIBRARIES
    NAMES gtest
    PATHS
      /usr/local/lib
      /usr/lib
      /opt/homebrew/lib
      $ENV{GTEST_ROOT}/lib
    NO_DEFAULT_PATH
  )
  if(NOT GTest_LIBRARIES)
    find_library(GTest_LIBRARIES NAMES gtest)
  endif()
endif()

if(NOT GTest_MAIN_LIBRARIES)
  find_library(GTest_MAIN_LIBRARIES
    NAMES gtest_main
    PATHS
      /usr/local/lib
      /usr/lib
      /opt/homebrew/lib
      $ENV{GTEST_ROOT}/lib
    NO_DEFAULT_PATH
  )
  if(NOT GTest_MAIN_LIBRARIES)
    find_library(GTest_MAIN_LIBRARIES NAMES gtest_main)
  endif()
endif()

# ---- 验证结果 ----
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GTest
  REQUIRED_VARS GTest_INCLUDE_DIRS GTest_LIBRARIES
)

# ---- 创建 IMPORTED 目标 ----
if(GTest_FOUND)
  # GTest 依赖 threads
  find_package(Threads QUIET)

  if(NOT TARGET GTest::gtest)
    add_library(GTest::gtest UNKNOWN IMPORTED)
    set_target_properties(GTest::gtest PROPERTIES
      IMPORTED_LOCATION             "${GTest_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${GTest_INCLUDE_DIRS}"
    )
    if(Threads_FOUND)
      set_target_properties(GTest::gtest PROPERTIES
        INTERFACE_LINK_LIBRARIES Threads::Threads
      )
    endif()
  endif()

  if(GTest_MAIN_LIBRARIES AND NOT TARGET GTest::gtest_main)
    add_library(GTest::gtest_main UNKNOWN IMPORTED)
    set_target_properties(GTest::gtest_main PROPERTIES
      IMPORTED_LOCATION             "${GTest_MAIN_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${GTest_INCLUDE_DIRS}"
      INTERFACE_LINK_LIBRARIES      GTest::gtest
    )
  endif()

  mark_as_advanced(GTest_INCLUDE_DIRS GTest_LIBRARIES GTest_MAIN_LIBRARIES)
endif()
