# tests/vk-encoder -- ctests for the GPU encoder passes.
#
# Every test that needs a GPU exits 77 when there is no ICD, no physical device,
# or a device that cannot meet the kernels' requirements (256-lane workgroups,
# subgroup width at least 8 per paper 3.2.6).  SKIP_RETURN_CODE turns that into
# a ctest skip rather than a failure, so the suite is green on a headless CI
# runner and on a developer box alike.

set(NXENC_TESTS)

if(TARGET nxvc-stats-test)
  # ------------------------------------------------------- E0 / E1 / E2
  # CPU models only: no Vulkan, so this one can never be skipped.  It is the
  # guard that the bit-exact reference itself still behaves after an edit.
  add_test(NAME vk.encoder.stats.cpu
           COMMAND nxvc-stats-test --cpu-only)

  # GPU vs CPU diff on the default device.  This is the test paper 3.9 calls the
  # definition of done for these kernels.
  add_test(NAME vk.encoder.stats.diff
           COMMAND nxvc-stats-test --device 0 --no-bench)
  set_tests_properties(vk.encoder.stats.diff PROPERTIES SKIP_RETURN_CODE 77)

  # A second physical device if the box has one -- on this developer machine that
  # is the integrated GPU, and in CI it is usually nothing.  Running the same diff
  # on two devices at once is cheap and is how a subgroup-width dependency would
  # first show up locally.
  add_test(NAME vk.encoder.stats.diff.device1
           COMMAND nxvc-stats-test --device 1 --no-bench)
  set_tests_properties(vk.encoder.stats.diff.device1 PROPERTIES SKIP_RETURN_CODE 77)

  # Same kernels under a second seed, so a passing run is not an artefact of one
  # particular set of test frames.
  add_test(NAME vk.encoder.stats.diff.seed2
           COMMAND nxvc-stats-test --device 0 --no-bench --seed 987654321)
  set_tests_properties(vk.encoder.stats.diff.seed2 PROPERTIES SKIP_RETURN_CODE 77)

  list(APPEND NXENC_TESTS vk.encoder.stats.cpu vk.encoder.stats.diff
              vk.encoder.stats.diff.device1 vk.encoder.stats.diff.seed2)
else()
  message(STATUS "tests/vk-encoder: nxvc-stats-test not built, no E0/E1/E2 tests")
endif()

# ------------------------------------------------- the C header / GLSL mirror
# Needs no target and no GPU: it reads the two source files.  nxe_enc.h and
# nxe_enc_common.glsl are hand-written copies of one contract, and a
# disagreement between them is silent -- NXE_LANE_OPS_CAP is a buffer stride
# shared between the host and the shader, so a drift there makes every rANS
# lane address another lane's scratch and quietly writes a wrong stream.
add_test(NAME vk.encoder.mirror
         COMMAND ${CMAKE_COMMAND}
         -DCHDR=${CMAKE_CURRENT_SOURCE_DIR}/../../vk/encoder/forward/nxe_enc.h
         -DGHDR=${CMAKE_CURRENT_SOURCE_DIR}/../../vk/encoder/forward/nxe_enc_common.glsl
         -P ${CMAKE_CURRENT_SOURCE_DIR}/mirror.cmake)
list(APPEND NXENC_TESTS vk.encoder.mirror)

# ------------------------------------------- the encoder's Pass W is the
# decoder's Pass W.  Also needs no target and no GPU: it compares the two
# generated SPIR-V modules word for word.  The encoder has no warp shader of
# its own -- it compiles vk/decoder/inter/warp_pred.comp -- and this is what
# keeps that true, because "by construction" is a claim this tree has been
# bitten by before.
set(_passw_dec
    ${CMAKE_BINARY_DIR}/vk/decoder/inter/generated/warp_pred.spv.h)
set(_passw_enc ${CMAKE_BINARY_DIR}/vk/encoder/generated/warp_pred.spv.h)
if(EXISTS ${CMAKE_SOURCE_DIR}/vk/decoder/inter/warp_pred.comp)
  add_test(NAME vk.encoder.passw.same
           COMMAND ${CMAKE_COMMAND}
           -DDEC=${_passw_dec} -DENC=${_passw_enc}
           -P ${CMAKE_CURRENT_SOURCE_DIR}/passw_same.cmake)
  list(APPEND NXENC_TESTS vk.encoder.passw.same)

  # The same check for Pass B, which is the other half of the rule: the
  # encoder's reference picture is reconstructed by the decoder's own module,
  # in its v1_x8 variant.
  add_test(NAME vk.encoder.passb.same
           COMMAND ${CMAKE_COMMAND}
           -DDEC=${CMAKE_BINARY_DIR}/vk/decoder/passB/generated/reconstruct_v1_x8.spv.h
           -DENC=${CMAKE_BINARY_DIR}/vk/encoder/generated/reconstruct_v1_x8.spv.h
           -P ${CMAKE_CURRENT_SOURCE_DIR}/passw_same.cmake)
  list(APPEND NXENC_TESTS vk.encoder.passb.same)
endif()

# The inter host module: the ring layout, the slot rule, the rolling refresh
# and the warp parameter buffer.  No GPU -- it is the bookkeeping that decides
# what the GPU is asked to do, and every rule in it is one the decoder
# implements from the same normative text, so a disagreement is a stream the
# decoder refuses.
# The inter acid test: byte-identity against nxv-enc at ADR-0028's integer
# decision, decode equality, and the ring against the decoder's own picture.
if(TARGET nxvc-vkenc AND TARGET nxv-enc AND TARGET nxv-dec)
  add_test(NAME vk.encoder.inter.acid.0
           COMMAND ${CMAKE_COMMAND}
           -DVKENC=$<TARGET_FILE:nxvc-vkenc>
           -DNXVENC=$<TARGET_FILE:nxv-enc>
           -DNXVDEC=$<TARGET_FILE:nxv-dec>
           -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/inter-acid-0
           -DVKENCAPI=$<IF:$<TARGET_EXISTS:nxvc-vkenc-api>,$<TARGET_FILE:nxvc-vkenc-api>,>
           -DDEVICE=0
           -P ${CMAKE_CURRENT_SOURCE_DIR}/inter_acid.cmake)
  set_tests_properties(vk.encoder.inter.acid.0 PROPERTIES
                       SKIP_REGULAR_EXPRESSION "SKIP:")
  list(APPEND NXENC_TESTS vk.encoder.inter.acid.0)

  # The same chain at the headset's own tile count.  The acid test above runs
  # at 256x192 -- 12 tiles -- and every other inter fixture in the tree is
  # 320x240 or smaller, so nothing here has ever crossed 256 tiles, which is
  # where a byte-wide tile index, a row bitmap or a prefix sum would first go
  # wrong.  This leg is 1088x1088: a 17x17 grid, 289 tiles, with the coded
  # vector searched, and it takes the stream all the way through the GPU
  # decoder's own parser.
  add_test(NAME vk.encoder.inter.cv1088
           COMMAND ${CMAKE_COMMAND}
           -DVKENC=$<TARGET_FILE:nxvc-vkenc>
           -DNXVENC=$<TARGET_FILE:nxv-enc>
           -DNXVDEC=$<TARGET_FILE:nxv-dec>
           -DVKDEC=$<IF:$<TARGET_EXISTS:nxvc-vkdec>,$<TARGET_FILE:nxvc-vkdec>,>
           -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/inter-cv1088
           -DVKENCAPI=$<IF:$<TARGET_EXISTS:nxvc-vkenc-api>,$<TARGET_FILE:nxvc-vkenc-api>,>
           -DDEVICE=0
           -P ${CMAKE_CURRENT_SOURCE_DIR}/inter_1088.cmake)
  set_tests_properties(vk.encoder.inter.cv1088 PROPERTIES
                       SKIP_REGULAR_EXPRESSION "SKIP:")
  list(APPEND NXENC_TESTS vk.encoder.inter.cv1088)

  # The same sequence at ENTROPY_LITE.  Inter is where the entropy tool and
  # the TILE HEADER meet: a coded-vector tile carries the optional vector
  # field between the header and a payload whose layout Lite changed, so a
  # mistake in the field size moves every byte of that tile.
  add_test(NAME vk.encoder.inter.acid.lite
           COMMAND ${CMAKE_COMMAND}
           -DVKENC=$<TARGET_FILE:nxvc-vkenc>
           -DNXVENC=$<TARGET_FILE:nxv-enc>
           -DNXVDEC=$<TARGET_FILE:nxv-dec>
           -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/inter-acid-lite
           -DVKENCAPI=$<IF:$<TARGET_EXISTS:nxvc-vkenc-api>,$<TARGET_FILE:nxvc-vkenc-api>,>
           -DDEVICE=0
           -DENTROPY=lite
           -P ${CMAKE_CURRENT_SOURCE_DIR}/inter_acid.cmake)
  set_tests_properties(vk.encoder.inter.acid.lite PROPERTIES
                       SKIP_REGULAR_EXPRESSION "SKIP:")
  list(APPEND NXENC_TESTS vk.encoder.inter.acid.lite)

  # The ATLAS reference (tool bit 31).  Byte-identity against nxv-enc, and --
  # the claim [SYN] 13.12 actually makes -- the encoder's shadow atlas against
  # the atlas nxv-dec builds from the GPU's own stream.  Both entropy tools,
  # because the atlas changes which tiles are coded and Lite changes what a
  # coded tile costs, so the two compose rather than stack.
  foreach(ent rans lite)
    add_test(NAME vk.encoder.atlas.acid.${ent}
             COMMAND ${CMAKE_COMMAND}
             -DVKENC=$<TARGET_FILE:nxvc-vkenc>
             -DNXVENC=$<TARGET_FILE:nxv-enc>
             -DNXVDEC=$<TARGET_FILE:nxv-dec>
             -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/atlas-acid-${ent}
             -DDEVICE=0
             -DENTROPY=${ent}
             -P ${CMAKE_CURRENT_SOURCE_DIR}/atlas_acid.cmake)
    set_tests_properties(vk.encoder.atlas.acid.${ent} PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:")
    list(APPEND NXENC_TESTS vk.encoder.atlas.acid.${ent})
  endforeach()

  # The atlas patch layout the C ABI reports, against the copies that consume
  # it.  The file says why a caller must not re-derive it.
  add_test(NAME vk.encoder.atlas.layout
           COMMAND ${CMAKE_COMMAND}
           -DVKENC=$<TARGET_FILE:nxvc-vkenc>
           -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/atlas-layout
           -DDEVICE=0
           -P ${CMAKE_CURRENT_SOURCE_DIR}/atlas_layout.cmake)
  set_tests_properties(vk.encoder.atlas.layout PROPERTIES
                       SKIP_REGULAR_EXPRESSION "SKIP:")
  list(APPEND NXENC_TESTS vk.encoder.atlas.layout)

  # [SYN] 13.12.11, the per-frame mode: an all-PICTURE stream must be the
  # no-atlas stream, byte for byte apart from the tool bits and the mode bit.
  add_test(NAME vk.encoder.atlas.mode
           COMMAND ${CMAKE_COMMAND}
           -DVKENC=$<TARGET_FILE:nxvc-vkenc>
           -DNXVDEC=$<TARGET_FILE:nxv-dec>
           -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/atlas-mode
           -DDEVICE=0
           -P ${CMAKE_CURRENT_SOURCE_DIR}/atlas_mode.cmake)
  set_tests_properties(vk.encoder.atlas.mode PROPERTIES
                       SKIP_REGULAR_EXPRESSION "SKIP:")
  list(APPEND NXENC_TESTS vk.encoder.atlas.mode)

  # row_present() (tool bit 32).  Against the reference DECODER, because the
  # reference ENCODER never sets frame flag bit 4 -- the file says why.
  add_test(NAME vk.encoder.rowpresent
           COMMAND ${CMAKE_COMMAND}
           -DVKENC=$<TARGET_FILE:nxvc-vkenc>
           -DNXVDEC=$<TARGET_FILE:nxv-dec>
           -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/rowpresent
           -DDEVICE=0
           -P ${CMAKE_CURRENT_SOURCE_DIR}/row_present.cmake)
  set_tests_properties(vk.encoder.rowpresent PROPERTIES
                       SKIP_REGULAR_EXPRESSION "SKIP:")
  list(APPEND NXENC_TESTS vk.encoder.rowpresent)
endif()

# The loop closed: what nxvc_vk_decoder -- the decoder that runs on the
# headset, and the whole reason ENTROPY_LITE exists -- makes of the streams
# the GPU encoder produced at tool bit 30.  Every other Lite test compares
# against nxv-enc or nxv-dec; this one compares the two GPU halves.
if(TARGET nxvc-vkenc AND TARGET nxvc-vkdec AND TARGET nxv-dec)
  add_test(NAME vk.encoder.lite.decode
           COMMAND ${CMAKE_COMMAND}
           -DVKENC=$<TARGET_FILE:nxvc-vkenc>
           -DVKDEC=$<TARGET_FILE:nxvc-vkdec>
           -DNXVDEC=$<TARGET_FILE:nxv-dec>
           -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/lite-decode
           -P ${CMAKE_CURRENT_SOURCE_DIR}/lite_decode.cmake)
  set_tests_properties(vk.encoder.lite.decode PROPERTIES
                       SKIP_REGULAR_EXPRESSION "SKIP:")
  list(APPEND NXENC_TESTS vk.encoder.lite.decode)
endif()

if(TARGET nxenc-inter-test)
  add_test(NAME vk.encoder.inter.host COMMAND nxenc-inter-test)
  list(APPEND NXENC_TESTS vk.encoder.inter.host)
endif()

# The ATLAS shadow: [SYN] 13.12.2's composition and 13.12.3's frame process
# against the clause's own arithmetic.  Needs no GPU and no reference codec.
if(TARGET nxenc-atlas-test)
  add_test(NAME vk.encoder.atlas.host COMMAND nxenc-atlas-test)
  list(APPEND NXENC_TESTS vk.encoder.atlas.host)
endif()

if(TARGET nxvc-vkenc)
  # -------------------------------------------------------- E3 / E4 / E5
  # The CPU models against their pinned stream digests.  No Vulkan, so it never
  # skips; it is what fails loudly if an edit changes the bitstream.
  add_test(NAME vk.encoder.forward.cpu
           COMMAND nxvc-vkenc --selftest --cpu)

  # The GPU pipeline against the CPU models: coefficients out of E3, then the
  # whole frame out of E4 and E5, over the same configuration table.
  add_test(NAME vk.encoder.forward.diff
           COMMAND nxvc-vkenc --selftest --device 0)
  set_tests_properties(vk.encoder.forward.diff PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME vk.encoder.forward.diff.device1
           COMMAND nxvc-vkenc --selftest --device 1)
  set_tests_properties(vk.encoder.forward.diff.device1 PROPERTIES SKIP_RETURN_CODE 77)

  list(APPEND NXENC_TESTS vk.encoder.forward.cpu vk.encoder.forward.diff
              vk.encoder.forward.diff.device1)

  # ------------------------------------------------------------ the acid test
  # A frame the GPU encoded must be byte-identical to the one the reference
  # encoder produces for the same decisions, and must decode with nxv-dec to
  # the same pixels.  Only available when the reference tools are in the build,
  # which they are from the repo root and are not in the standalone encoder
  # build; the digest test above is what covers the standalone case.
  if(TARGET nxv-enc AND TARGET nxv-dec)
    foreach(dev cpu 0)
      set(_name vk.encoder.acid.${dev})
      add_test(NAME ${_name} COMMAND ${CMAKE_COMMAND}
               -DVKENC=$<TARGET_FILE:nxvc-vkenc>
               -DNXVENC=$<TARGET_FILE:nxv-enc>
               -DNXVDEC=$<TARGET_FILE:nxv-dec>
               -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/acid-${dev}
               -DDEVICE=${dev}
               -P ${CMAKE_CURRENT_SOURCE_DIR}/acid.cmake)
      set_tests_properties(${_name} PROPERTIES
                           SKIP_REGULAR_EXPRESSION "SKIP:")
      list(APPEND NXENC_TESTS ${_name})
    endforeach()
    # The same table again at ENTROPY_LITE (tool bit 30).  It is a separate
    # test rather than a wider loop because it is a different STREAM, not a
    # different setting: Lite turns sign hiding, custom tables and TAB_V2 off
    # in both encoders, so a run that mixed the two would be comparing two
    # things and reporting one number.
    foreach(dev cpu 0)
      set(_name vk.encoder.acid.lite.${dev})
      add_test(NAME ${_name} COMMAND ${CMAKE_COMMAND}
               -DVKENC=$<TARGET_FILE:nxvc-vkenc>
               -DNXVENC=$<TARGET_FILE:nxv-enc>
               -DNXVDEC=$<TARGET_FILE:nxv-dec>
               -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/acid-lite-${dev}
               -DDEVICE=${dev}
               -DENTROPY=lite
               -P ${CMAKE_CURRENT_SOURCE_DIR}/acid.cmake)
      set_tests_properties(${_name} PROPERTIES
                           SKIP_REGULAR_EXPRESSION "SKIP:")
      list(APPEND NXENC_TESTS ${_name})
    endforeach()

    # The same claim for the C ABI, which acid.cmake does not reach: the
    # library's own configuration mapping is what an integrator links.
    if(TARGET nxvc-vkenc-api)
      add_test(NAME vk.encoder.acid.api COMMAND ${CMAKE_COMMAND}
               -DAPIENC=$<TARGET_FILE:nxvc-vkenc-api>
               -DVKENC=$<TARGET_FILE:nxvc-vkenc>
               -DNXVENC=$<TARGET_FILE:nxv-enc>
               -DNXVDEC=$<TARGET_FILE:nxv-dec>
               -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/acid-api
               -P ${CMAKE_CURRENT_SOURCE_DIR}/api_acid.cmake)
      set_tests_properties(vk.encoder.acid.api PROPERTIES
                           SKIP_REGULAR_EXPRESSION "SKIP:")
      list(APPEND NXENC_TESTS vk.encoder.acid.api)

      # And the ABI's own entropy switch: `entropy = NXVC_VKE_ENTROPY_LITE`
      # has to take three tools out of the stream header as well as out of the
      # coding, which is a mapping only a byte comparison checks.
      add_test(NAME vk.encoder.acid.api.lite COMMAND ${CMAKE_COMMAND}
               -DAPIENC=$<TARGET_FILE:nxvc-vkenc-api>
               -DVKENC=$<TARGET_FILE:nxvc-vkenc>
               -DNXVENC=$<TARGET_FILE:nxv-enc>
               -DNXVDEC=$<TARGET_FILE:nxv-dec>
               -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/acid-api-lite
               -DENTROPY=lite
               -P ${CMAKE_CURRENT_SOURCE_DIR}/api_acid.cmake)
      set_tests_properties(vk.encoder.acid.api.lite PROPERTIES
                           SKIP_REGULAR_EXPRESSION "SKIP:")
      list(APPEND NXENC_TESTS vk.encoder.acid.api.lite)

      # The same claim for the image entry point, which is the one a
      # compositor actually calls: E0 reads a two-plane VkImage on the device
      # and no plane is laid out on the host.  It shares every pass after E0
      # with the plane path, so this test is what says the sharing is real.
      add_test(NAME vk.encoder.acid.api.image COMMAND ${CMAKE_COMMAND}
               -DAPIENC=$<TARGET_FILE:nxvc-vkenc-api>
               -DVKENC=$<TARGET_FILE:nxvc-vkenc>
               -DNXVENC=$<TARGET_FILE:nxv-enc>
               -DNXVDEC=$<TARGET_FILE:nxv-dec>
               -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/acid-api-image
               -DIMAGE=ON
               -P ${CMAKE_CURRENT_SOURCE_DIR}/api_acid.cmake)
      set_tests_properties(vk.encoder.acid.api.image PROPERTIES
                           SKIP_REGULAR_EXPRESSION "SKIP:")
      list(APPEND NXENC_TESTS vk.encoder.acid.api.image)

      # The eye PAIR, through the same ABI.  `eyes = 2` codes both eyes of a
      # frame as one nxvc stereo frame in one submission, which is the shape
      # the decoder wants -- and it is the configuration in which two latent
      # per-eye/pair confusions lived: E1c's search took its origin at
      # `eye * ring_w` with `ring_w` the PAIR width, and E0 was handed the
      # per-eye geometry and converted eye 0 only.  Both are invisible at
      # `eyes == 1`, where the two widths coincide, so only a stereo row of
      # the selftest table can hold them down.  The image leg matters most:
      # E0 runs on that path alone.
      foreach(leg plain image)
        set(stereo_extra)
        if(leg STREQUAL "image")
          set(stereo_extra -DIMAGE=ON)
        endif()
        add_test(NAME vk.encoder.acid.api.stereo.${leg} COMMAND ${CMAKE_COMMAND}
                 -DAPIENC=$<TARGET_FILE:nxvc-vkenc-api>
                 -DVKENC=$<TARGET_FILE:nxvc-vkenc>
                 -DNXVENC=$<TARGET_FILE:nxv-enc>
                 -DNXVDEC=$<TARGET_FILE:nxv-dec>
                 -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/acid-api-stereo-${leg}
                 -DEYES=2 ${stereo_extra}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/api_acid.cmake)
        set_tests_properties(vk.encoder.acid.api.stereo.${leg} PROPERTIES
                             SKIP_REGULAR_EXPRESSION "SKIP:")
        list(APPEND NXENC_TESTS vk.encoder.acid.api.stereo.${leg})
      endforeach()

      # The eyes as separate ARRAY LAYERS.  This is the shape a compositor
      # actually has -- WiVRn's eyes are layers of one image, and so are
      # OpenXR's projection views in general -- and the reason the flag exists
      # is that without it such a caller has to copy both layers into one
      # side-by-side image before every encode, a full-frame blit per frame to
      # satisfy a layout this pass can read directly.  The claim that makes it
      # safe is that the two shapes are the SAME STREAM, so it is pinned against
      # the same reference every other leg uses.
      add_test(NAME vk.encoder.acid.api.stereo.layers COMMAND ${CMAKE_COMMAND}
               -DAPIENC=$<TARGET_FILE:nxvc-vkenc-api>
               -DVKENC=$<TARGET_FILE:nxvc-vkenc>
               -DNXVENC=$<TARGET_FILE:nxv-enc>
               -DNXVDEC=$<TARGET_FILE:nxv-dec>
               -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/acid-api-stereo-layers
               -DEYES=2 -DIMAGE=ON -DEYE_LAYERS=ON
               -P ${CMAKE_CURRENT_SOURCE_DIR}/api_acid.cmake)
      set_tests_properties(vk.encoder.acid.api.stereo.layers PROPERTIES
                           SKIP_REGULAR_EXPRESSION "SKIP:")
      list(APPEND NXENC_TESTS vk.encoder.acid.api.stereo.layers)
      # The effort levels: the integer requantiser on both sides of the
      # byte-identity line, and the refusal of a level that does not exist.
      # It is registered next to the ABI tests because half of it is the
      # ABI's own mapping of `create_info::effort`.
      foreach(dev cpu 0)
        add_test(NAME vk.encoder.acid.effort.${dev} COMMAND ${CMAKE_COMMAND}
                 -DVKENC=$<TARGET_FILE:nxvc-vkenc>
                 -DNXVENC=$<TARGET_FILE:nxv-enc>
                 -DNXVDEC=$<TARGET_FILE:nxv-dec>
                 -DVKENCAPI=$<IF:$<TARGET_EXISTS:nxvc-vkenc-api>,$<TARGET_FILE:nxvc-vkenc-api>,>
                 -DVKDEC=$<IF:$<TARGET_EXISTS:nxvc-vkdec>,$<TARGET_FILE:nxvc-vkdec>,>
                 -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/effort-${dev}
                 -DDEVICE=${dev}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/effort.cmake)
        set_tests_properties(vk.encoder.acid.effort.${dev} PROPERTIES
                             SKIP_REGULAR_EXPRESSION "SKIP:")
        list(APPEND NXENC_TESTS vk.encoder.acid.effort.${dev})
      endforeach()

      # And the same legs on a SECOND ICD.  Byte-identity is a claim about
      # the arithmetic, not about RADV: the requantiser's one 64-bit product
      # goes through umulExtended and its comparison through a signed 32-bit
      # subtraction of squares, both of which a different compiler is free to
      # lower differently.  lavapipe is the second implementation this tree
      # already keeps for the decoder (tests/vk), and it is where a vendor
      # difference would show up as a stream that is not the reference's.
      if(NXVC_LAVAPIPE_ICD AND EXISTS "${NXVC_LAVAPIPE_ICD}")
        add_test(NAME vk.encoder.acid.effort.lavapipe COMMAND ${CMAKE_COMMAND}
                 -DVKENC=$<TARGET_FILE:nxvc-vkenc>
                 -DNXVENC=$<TARGET_FILE:nxv-enc>
                 -DNXVDEC=$<TARGET_FILE:nxv-dec>
                 -DVKENCAPI=$<IF:$<TARGET_EXISTS:nxvc-vkenc-api>,$<TARGET_FILE:nxvc-vkenc-api>,>
                 -DVKDEC=$<IF:$<TARGET_EXISTS:nxvc-vkdec>,$<TARGET_FILE:nxvc-vkdec>,>
                 -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/effort-lavapipe
                 -DDEVICE=0
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/effort.cmake)
        set_tests_properties(vk.encoder.acid.effort.lavapipe PROPERTIES
                             SKIP_REGULAR_EXPRESSION "SKIP:"
                             ENVIRONMENT
                             "VK_DRIVER_FILES=${NXVC_LAVAPIPE_ICD};VK_ICD_FILENAMES=${NXVC_LAVAPIPE_ICD}")
        list(APPEND NXENC_TESTS vk.encoder.acid.effort.lavapipe)
      endif()

      # The same claim for a quantiser that MOVES, which is the entry point a
      # rate controller uses and the one the two tests above never touch: after
      # nxvc_vk_encoder_set_qp(q), every frame must be the frame nxv-enc codes
      # at q AND at that frame number.  Frame by frame, over a cycle of four
      # quantisers, from an encoder created at a fifth.
      add_test(NAME vk.encoder.acid.api.qp_switch COMMAND ${CMAKE_COMMAND}
               -DAPIENC=$<TARGET_FILE:nxvc-vkenc-api>
               -DVKENC=$<TARGET_FILE:nxvc-vkenc>
               -DNXVENC=$<TARGET_FILE:nxv-enc>
               -DNXVDEC=$<TARGET_FILE:nxv-dec>
               -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/qp-switch
               -P ${CMAKE_CURRENT_SOURCE_DIR}/qp_switch.cmake)
      set_tests_properties(vk.encoder.acid.api.qp_switch PROPERTIES
                           SKIP_REGULAR_EXPRESSION "SKIP:")
      list(APPEND NXENC_TESTS vk.encoder.acid.api.qp_switch)
    endif()
  else()
    message(STATUS "tests/vk-encoder: nxv-enc/nxv-dec not built, no acid test")
  endif()
else()
  message(STATUS "tests/vk-encoder: nxvc-vkenc not built, no E3/E4/E5 tests")
endif()

if(NXENC_TESTS)
  set_tests_properties(${NXENC_TESTS} PROPERTIES TIMEOUT 900 LABELS "vk-encoder")
endif()
