-- include subprojects
includes("lib/commonlibsf")

-- set project constants
set_project("ImmersiveWeaponSwitching")
set_version("1.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- MinHook from the official xmake repo
add_requires("minhook")

-- define targets
target("ImmersiveWeaponSwitching")
    add_rules("commonlibsf.plugin", {
        name = "ImmersiveWeaponSwitching",
        author = "Momo",
        description = "Immersive weapon switching for Starfield",
        email = "momomanbavaran@gmail.com"
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- link MinHook
    add_packages("minhook")