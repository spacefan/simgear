include (SimGearComponent)

set(HEADERS 
    sample.hxx
    sample_group.hxx
    xmlsound.hxx
    soundmgr.hxx
    filters.hxx
    )
    
set(SOURCES 
    sample.cxx
    sample_group.cxx
    xmlsound.cxx
    filters.cxx
    )

if (USE_AEONWAVE)
    set(SOURCES ${SOURCES}
        soundmgr_aeonwave.cxx
        )
else()
    set(HEADERS ${HEADERS}
        readwav.hxx
        )
    set(SOURCES ${SOURCES}
        soundmgr_openal.cxx
        soundmgr_openal_private.hxx
        readwav.cxx
        )
endif()

simgear_scene_component(sound sound "${SOURCES}" "${HEADERS}")

if(ENABLE_TESTS AND ENABLE_SOUND)
    function(create_test TEST_NAME)
        add_executable(${TEST_NAME} ${TEST_NAME}.cxx)
        target_link_libraries(${TEST_NAME} ${SOUND_TEST_LIBS})
        set_target_properties(${TEST_NAME} PROPERTIES
            COMPILE_DEFINITIONS "SRC_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"" )
    endfunction()

    set( SOUND_TEST_LIBS
        ${TEST_LIBS}
    )

    if (USE_AEONWAVE)
        if (SIMGEAR_SHARED)
        else()
            set(SOUND_TEST_LIBS ${SOUND_TEST_LIBS}
                ${AAX_LIBRARY}
            )
        endif()

        create_test(aeonwave_test1)

    else ()
        if (SIMGEAR_SHARED)
        else()
            set(SOUND_TEST_LIBS ${SOUND_TEST_LIBS}
                ${OPENAL_LIBRARY}
            )
        endif()

        create_test(openal_test1)
    endif()

    create_test(soundmgr_test)
    create_test(soundmgr_test2)
endif()
