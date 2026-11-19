# hifi (patch 0038): libogg, Tremor, opus and libFLAC built from the pinned
# sources in sources.lock, for CONFIG_HIFI_PUBLISHABLE_CODECS (the makers'
# image), in place of the prebuilt lib/libogg.a, libvorbisidec.a, libopus.a
# and libFLAC.a, whose sources upstream does not record. fetch.sh unpacks
# each source into this folder and gathers the public headers in include/.
# components/codecs/CMakeLists.txt adds HIFI_XIPH_SRCS to its SRCS and
# HIFI_XIPH_INCLUDE to its INCLUDE_DIRS, then calls hifi_xiph_flags().
set(HIFI_XIPH ${CMAKE_CURRENT_LIST_DIR})
set(HIFI_XIPH_INCLUDE ${HIFI_XIPH}/include)

set(HIFI_OGG_SRCS ${HIFI_XIPH}/ogg/src/bitwise.c ${HIFI_XIPH}/ogg/src/framing.c)

set(HIFI_TREMOR_SRCS)
foreach(f block codebook floor0 floor1 info mapping0 mdct registry res012 sharedbook synthesis vorbisfile window)
	list(APPEND HIFI_TREMOR_SRCS ${HIFI_XIPH}/tremor/${f}.c)
endforeach()

set(HIFI_FLAC_SRCS)
foreach(f bitmath bitreader cpu crc fixed format lpc md5 memory ogg_decoder_aspect ogg_mapping stream_decoder)
	list(APPEND HIFI_FLAC_SRCS ${HIFI_XIPH}/flac/src/libFLAC/${f}.c)
endforeach()

# opus's own lists (opus_sources.mk and the others), as its CMakeLists reads them
function(hifi_opus_list mk var out)
	file(READ ${HIFI_XIPH}/opus/${mk} text)
	string(REGEX REPLACE "\\\\\n" " " text "${text}")
	string(REGEX MATCH "(^|\n)${var} = [^\n]*" line "${text}")
	if(NOT line)
		message(FATAL_ERROR "xiph.cmake: no ${var} in opus/${mk}")
	endif()
	string(REGEX REPLACE "(^|\n)${var} = " "" line "${line}")
	separate_arguments(files UNIX_COMMAND "${line}")
	set(paths)
	foreach(f ${files})
		list(APPEND paths ${HIFI_XIPH}/opus/${f})
	endforeach()
	set(${out} ${paths} PARENT_SCOPE)
endfunction()
hifi_opus_list(opus_sources.mk OPUS_SOURCES opus_a)
hifi_opus_list(celt_sources.mk CELT_SOURCES opus_b)
hifi_opus_list(silk_sources.mk SILK_SOURCES opus_c)
hifi_opus_list(silk_sources.mk SILK_SOURCES_FIXED opus_d)
set(HIFI_OPUS_SRCS ${opus_a} ${opus_b} ${opus_c} ${opus_d})

set(HIFI_XIPH_SRCS ${HIFI_OGG_SRCS} ${HIFI_TREMOR_SRCS} ${HIFI_FLAC_SRCS} ${HIFI_OPUS_SRCS})

# Each library's own defines and private include folders, set per source
# file so that they don't reach the rest of the component. The settings are
# the prebuilt ones' where they can be told (README.md).
function(hifi_xiph_flags)
	set_source_files_properties(${HIFI_TREMOR_SRCS} PROPERTIES
		INCLUDE_DIRECTORIES "${HIFI_XIPH}/tremor")
	set_source_files_properties(${HIFI_FLAC_SRCS} PROPERTIES
		INCLUDE_DIRECTORIES "${HIFI_XIPH}/flac/include;${HIFI_XIPH}/flac/src/libFLAC/include"
		COMPILE_DEFINITIONS "FLAC__HAS_OGG=1;OGG_FOUND=1;FLAC__NO_DLL;PACKAGE_VERSION=\"1.5.0\";CPU_IS_BIG_ENDIAN=0;CPU_IS_LITTLE_ENDIAN=1;ENABLE_64_BIT_WORDS=0;HAVE_LROUND=1;HAVE_STDINT_H=1;HAVE_INTTYPES_H=1;FLAC__NO_ASM")
	set_source_files_properties(${HIFI_OPUS_SRCS} PROPERTIES
		INCLUDE_DIRECTORIES "${HIFI_XIPH}/opus/include;${HIFI_XIPH}/opus;${HIFI_XIPH}/opus/celt;${HIFI_XIPH}/opus/silk;${HIFI_XIPH}/opus/silk/fixed"
		COMPILE_DEFINITIONS "OPUS_BUILD;FIXED_POINT;DISABLE_FLOAT_API;NONTHREADSAFE_PSEUDOSTACK;HAVE_LRINT=1;HAVE_LRINTF=1;PACKAGE_VERSION=\"1.5.2\"")
	set_source_files_properties(${HIFI_XIPH_SRCS} PROPERTIES
		COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-unused-function;-Wno-implicit-fallthrough;-Wno-maybe-uninitialized;-Wno-stringop-overflow;-Wno-restrict")
endfunction()
