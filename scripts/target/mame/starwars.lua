-- license:BSD-3-Clause
-- copyright-holders:MAMEdev Team

---------------------------------------------------------------------------
--
--   starwars.lua
--
--   Atari Star Wars specific build target
--   Use make SUBTARGET=starwars to build
--
---------------------------------------------------------------------------


--------------------------------------------------
-- Specify all the CPU cores necessary for the
-- drivers referenced in starwars.lst.
--------------------------------------------------

CPUS["M6809"] = true


--------------------------------------------------
-- Specify all the sound cores necessary for the
-- drivers referenced in starwars.lst.
--------------------------------------------------

SOUNDS["POKEY"] = true
SOUNDS["TMS5220"] = true


--------------------------------------------------
-- specify available video cores
--------------------------------------------------

VIDEOS["AVGDVG"] = true


--------------------------------------------------
-- specify available machine cores
--------------------------------------------------

MACHINES["ADC0808"] = true
MACHINES["BANKDEV"] = true
MACHINES["GEN_LATCH"] = true
MACHINES["MOS6530"] = true
MACHINES["TTL74259"] = true
MACHINES["WATCHDOG"] = true
MACHINES["X2212"] = true


--------------------------------------------------
-- This is the list of files that are necessary
-- for building the Atari Star Wars driver.
--------------------------------------------------

function createProjects_mame_starwars(_target, _subtarget)
	project ("mame_starwars")
	targetsubdir(_target .."_" .. _subtarget)
	kind (LIBTYPE)
	uuid (os.uuid("drv-mame-starwars"))
	addprojectflags()
	precompiledheaders_novs()

	includedirs {
		MAME_DIR .. "src/osd",
		MAME_DIR .. "src/emu",
		MAME_DIR .. "src/devices",
		MAME_DIR .. "src/mame/shared",
		MAME_DIR .. "src/lib",
		MAME_DIR .. "src/lib/util",
		MAME_DIR .. "3rdparty",
		GEN_DIR  .. "mame/layout",
	}

	includedirs {
		ext_includedir("asio"),
		ext_includedir("flac"),
		ext_includedir("glm"),
		ext_includedir("jpeg"),
		ext_includedir("rapidjson"),
		ext_includedir("zlib")
	}

	files {
		MAME_DIR .. "src/mame/atari/slapstic.cpp",
		MAME_DIR .. "src/mame/atari/slapstic.h",
		MAME_DIR .. "src/mame/atari/starwars.cpp",
		MAME_DIR .. "src/mame/atari/starwars.h",
		MAME_DIR .. "src/mame/atari/starwars_m.cpp",
	}
end

function linkProjects_mame_starwars(_target, _subtarget)
	links {
		"mame_starwars",
	}
end