# C++20 domain/JSON units shared by the host and the asset-free launcher suite. No ReXGlue or ImGui.
set(GTAV_WORKSHOP_CORE
    ${CMAKE_CURRENT_LIST_DIR}/src/core/PermissionManager.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/core/PlayerRegistry.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/core/GamemodeManager.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/core/CheatController.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/core/EntitySpawnQueue.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/core/WorkshopCore.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/sandbox/BuildManager.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/sandbox/CreatorManager.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/utils/ConfigStorage.cpp)
