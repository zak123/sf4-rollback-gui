cmake_minimum_required (VERSION 3.20)

project (
    sf4e
    VERSION 0.1.8
    DESCRIPTION "A process-inspection and modification tool for the Steam release of Ultra Street Fighter 4."
    HOMEPAGE_URL "https://gitlab.com/adanducci/sf4e"
)
enable_language(RC)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG")
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)
add_compile_definitions(WIN32_LEAN_AND_MEAN)
# The session handshake compares client and server versions to warn
# players on stale builds, so the project VERSION above must keep being
# bumped alongside releases.
add_compile_definitions(SF4E_VERSION="${PROJECT_VERSION}")

find_library(LIB_DETOURS "detours")
find_package(CLI11 CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)
find_package(ggpo CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

find_path(VALVEFILEVDF_INCLUDE_DIRS "vdf_parser.hpp")

# Stamp Windows version metadata (publisher, description, version) on
# the shipped binaries. Metadata-less unsigned executables score worse
# with Defender's heuristics and show nothing in file properties.
# Bump the project VERSION above alongside releases.
function(sf4e_add_version_resource target filetype description filename)
    set(VER_FILETYPE "${filetype}")
    set(VER_DESCRIPTION "${description}")
    set(VER_FILENAME "${filename}")
    configure_file(
        "${CMAKE_SOURCE_DIR}/src/version.rc.in"
        "${CMAKE_BINARY_DIR}/version_rc/${target}.rc"
        @ONLY
    )
    target_sources(${target} PRIVATE "${CMAKE_BINARY_DIR}/version_rc/${target}.rc")
endfunction()

add_library(Dimps STATIC
    "src/Dimps/Dimps.hxx"
    "src/Dimps/Dimps.cxx"
    "src/Dimps/Dimps__Eva.hxx"
    "src/Dimps/Dimps__Eva.cxx"
    "src/Dimps/Dimps__Event.hxx"
    "src/Dimps/Dimps__Event.cxx"
    "src/Dimps/Dimps__Game.hxx"
    "src/Dimps/Dimps__Game.cxx"
    "src/Dimps/Dimps__Game__Battle.hxx"
    "src/Dimps/Dimps__Game__Battle.cxx"
    "src/Dimps/Dimps__Game__Battle__Action.hxx"
    "src/Dimps/Dimps__Game__Battle__Action.cxx"
    "src/Dimps/Dimps__Game__Battle__Camera.hxx"
    "src/Dimps/Dimps__Game__Battle__Camera.cxx"
    "src/Dimps/Dimps__Game__Battle__Chara.hxx"
    "src/Dimps/Dimps__Game__Battle__Chara.cxx"
    "src/Dimps/Dimps__Game__Battle__Command.hxx"
    "src/Dimps/Dimps__Game__Battle__Command.cxx"
    "src/Dimps/Dimps__Game__Battle__Effect.hxx"
    "src/Dimps/Dimps__Game__Battle__Effect.cxx"
    "src/Dimps/Dimps__Game__Battle__Hud.hxx"
    "src/Dimps/Dimps__Game__Battle__Hud.cxx"
    "src/Dimps/Dimps__Game__Battle__System.hxx"
    "src/Dimps/Dimps__Game__Battle__System.cxx"
    "src/Dimps/Dimps__Game__Battle__Training.hxx"
    "src/Dimps/Dimps__Game__Battle__Training.cxx"
    "src/Dimps/Dimps__Game__Battle__Vfx.hxx"
    "src/Dimps/Dimps__Game__Battle__Vfx.cxx"
    "src/Dimps/Dimps__GameEvents.hxx"
    "src/Dimps/Dimps__GameEvents.cxx"
    "src/Dimps/Dimps__Math.hxx"
    "src/Dimps/Dimps__Math.cxx"
    "src/Dimps/Dimps__Pad.hxx"
    "src/Dimps/Dimps__Pad.cxx"
    "src/Dimps/Dimps__Platform.hxx"
    "src/Dimps/Dimps__Platform.cxx"
    "src/Dimps/Dimps__UserApp.hxx"
    "src/Dimps/Dimps__UserApp.cxx"
)
target_link_libraries(
    Dimps
    PUBLIC nlohmann_json::nlohmann_json
)

add_library(sf4e STATIC
    "src/sf4e/sf4e.hxx"
    "src/sf4e/sf4e.cxx"
    "src/sf4e/sf4e__Event.hxx"
    "src/sf4e/sf4e__Event.cxx"
    "src/sf4e/sf4e__Game.hxx"
    "src/sf4e/sf4e__Game.cxx"
    "src/sf4e/sf4e__Game__Battle.hxx"
    "src/sf4e/sf4e__Game__Battle.cxx"
    "src/sf4e/sf4e__Game__Battle__Effect.hxx"
    "src/sf4e/sf4e__Game__Battle__Effect.cxx"
    "src/sf4e/sf4e__Game__Battle__Hud.hxx"
    "src/sf4e/sf4e__Game__Battle__Hud.cxx"
    "src/sf4e/sf4e__Game__Battle__System.hxx"
    "src/sf4e/sf4e__Game__Battle__System.cxx"
    "src/sf4e/sf4e__Game__Battle__Vfx.hxx"
    "src/sf4e/sf4e__Game__Battle__Vfx.cxx"
    "src/sf4e/sf4e__GameEvents.hxx"
    "src/sf4e/sf4e__GameEvents.cxx"
    "src/sf4e/sf4e__Overlay.hxx"
    "src/sf4e/sf4e__Overlay.cxx"
    "src/sf4e/sf4e__Pad.hxx"
    "src/sf4e/sf4e__Pad.cxx"
    "src/sf4e/sf4e__Platform.hxx"
    "src/sf4e/sf4e__Platform.cxx"
    "src/sf4e/sf4e__UserApp.hxx"
    "src/sf4e/sf4e__UserApp.cxx"
)
target_link_libraries(
    sf4e
    PRIVATE ${LIB_DETOURS} Session "Kernel32" "ws2_32" "Mswsock" "AdvApi32" "Shlwapi" "Bcrypt" "Xinput9_1_0"
    PUBLIC imgui::imgui nlohmann_json::nlohmann_json spdlog::spdlog GameNetworkingSockets::shared ggpo::GGPO
)

add_library(Session STATIC
    "src/session/sf4e__LobbyRegistry.hxx"
    "src/session/sf4e__LobbyRegistry.cxx"
    "src/session/sf4e__Resolve.hxx"
    "src/session/sf4e__Resolve.cxx"
    "src/session/sf4e__SessionClient.hxx"
    "src/session/sf4e__SessionClient.cxx"
    "src/session/sf4e__SessionProtocol.hxx"
    "src/session/sf4e__SessionProtocol.cxx"
    "src/session/sf4e__SessionServer.hxx"
    "src/session/sf4e__SessionServer.cxx"
)
target_link_libraries(
    Session
    PRIVATE Dimps
    PUBLIC nlohmann_json::nlohmann_json spdlog::spdlog GameNetworkingSockets::shared "ws2_32"
)

add_executable(Lobbyd "src/lobbyd/lobbyd.cxx")
target_link_libraries(Lobbyd PRIVATE Session CLI11::CLI11)

add_executable(SessionSmokeTest "src/session/smoke_test.cxx")
target_link_libraries(SessionSmokeTest PRIVATE nlohmann_json::nlohmann_json imgui::imgui spdlog::spdlog Dimps sf4e Session "d3d9")

add_executable(LobbyClient WIN32
    "src/client/lobby_client.cxx"
    "src/client/sf4e__Roster.hxx"
)
target_link_libraries(LobbyClient PRIVATE nlohmann_json::nlohmann_json CLI11::CLI11 imgui::imgui spdlog::spdlog Dimps sf4e Session "d3d9" "shlwapi" "winmm")
target_compile_definitions(LobbyClient PRIVATE -D_UNICODE -DUNICODE)
# The challenge/matchmake notification sound ships beside the exe.
add_custom_command(TARGET LobbyClient POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/client/notify.wav"
        "$<TARGET_FILE_DIR:LobbyClient>/notify.wav"
)

add_executable(SessionInteractiveTest "src/session/interactive_test.cxx")
target_link_libraries(SessionInteractiveTest PRIVATE nlohmann_json::nlohmann_json imgui::imgui spdlog::spdlog Dimps sf4e Session "d3d9")
# target_link_options(SessionInteractiveTest PRIVATE "/entry:wWinMainCRTStartup")

add_executable(Launcher WIN32 src/launcher/launcher.cxx)
target_include_directories(Launcher PRIVATE ${VALVEFILEVDF_INCLUDE_DIRS})
target_link_libraries(Launcher PRIVATE ${LIB_DETOURS} CLI11::CLI11 spdlog::spdlog "shlwapi" "pathcch")

add_library(Sidecar SHARED src/sidecar/sidecar.cxx)
target_include_directories(Sidecar PRIVATE ${VALVEFILEVDF_INCLUDE_DIRS})
target_link_libraries(Sidecar PRIVATE ${LIB_DETOURS} Dimps sf4e)

target_compile_definitions(Launcher PRIVATE -D_UNICODE -DUNICODE)

sf4e_add_version_resource(Launcher 0x1L "sf4e rollback netplay launcher for USF4" "Launcher.exe")
sf4e_add_version_resource(Sidecar 0x2L "sf4e rollback netplay sidecar for USF4" "Sidecar.dll")
sf4e_add_version_resource(LobbyClient 0x1L "sf4e lobby client" "LobbyClient.exe")
sf4e_add_version_resource(Lobbyd 0x1L "sf4e dedicated lobby server" "Lobbyd.exe")

install(
  TARGETS Launcher Sidecar
  RUNTIME DESTINATION "."
)

set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VENDOR "The ${PROJECT_NAME} Dev Team")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_NAME} ${PROJECT_DESCRIPTION}")
