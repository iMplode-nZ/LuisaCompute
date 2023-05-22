target("lc-backend-metal")
_config_project({
	project_kind = "shared",
	batch_size = 8
})
add_deps("lc-runtime", "lc-gui")
add_files("*.cpp")
add_files("metal_swapchain.mm")
add_frameworks("Foundation", "Metal", "QuartzCore", "AppKit")
target_end()
