# Borrowed-target dirty catchup prototype: rejected

The opt-in generation API, decoder dirty-history implementation, three-target test changes, and client wiring are preserved in the adjacent patches. The prototype was rejected: the Vulkan test crashed in CPU staging memcpy at `vk/decoder/nxvc_vkdec.cpp:3088` before submission when the dirty path was exercised. Reproduction used `nxvc-atlas-borrowed-r8-test` with a five-frame 0->1->2->0->1 target sequence and `NXVC_VKD_ATLAS_VIEW_DIRTY=1`; no performance or correctness result is claimed.
