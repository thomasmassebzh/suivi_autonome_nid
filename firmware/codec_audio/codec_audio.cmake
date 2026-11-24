# Choix de la version d'esp_audio_codec selon la révision de l'ESP32-P4.
# esp_audio_codec 2.6 et plus exige une puce rév. ≥ 3.0 ; une puce rév. 1.x
# (CONFIG_ESP32P4_SELECTS_REV_LESS_V3) a besoin de la 2.4 / 2.5. Une condition
# sur la configuration dans idf_component.yml n'est pas évaluée à temps par le
# gestionnaire de composants : le choix se fait ici, avant project(), d'après
#  - la variable NID_PUCE_REV1 (idf.py -D NID_PUCE_REV1=1 ...),
#  - les fichiers de valeurs par défaut (sdkconfig.defaults.rev1),
#  - ou le sdkconfig existant.
set(_nid_rev1 OFF)
if(NID_PUCE_REV1)
    set(_nid_rev1 ON)
elseif(DEFINED SDKCONFIG_DEFAULTS AND "${SDKCONFIG_DEFAULTS}" MATCHES "rev1")
    set(_nid_rev1 ON)
else()
    if(DEFINED SDKCONFIG)
        set(_nid_sdkconfig "${SDKCONFIG}")
    else()
        set(_nid_sdkconfig "${CMAKE_SOURCE_DIR}/sdkconfig")
    endif()
    if(EXISTS "${_nid_sdkconfig}")
        file(STRINGS "${_nid_sdkconfig}" _nid_rev_line REGEX "^CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y")
        if(_nid_rev_line)
            set(_nid_rev1 ON)
        endif()
    endif()
endif()
set(_nid_codec_dir "${CMAKE_CURRENT_LIST_DIR}")
if(_nid_rev1)
    message(STATUS "Suivi nid : puce ESP32-P4 rév. 1.x, esp_audio_codec 2.4/2.5")
    list(APPEND EXTRA_COMPONENT_DIRS "${_nid_codec_dir}/v24/nid_codec_audio")
else()
    list(APPEND EXTRA_COMPONENT_DIRS "${_nid_codec_dir}/v26/nid_codec_audio")
endif()
