##
## Update this file as simulator package-family metadata changes.
## Keep CPack component declarations and other packaging-specific metadata
## here. Do not regenerate this file from the legacy generator.

## The default runtime support component/family:
cpack_add_component(runtime_support
    DISPLAY_NAME "Runtime support"
    DESCRIPTION "Required SIMH runtime support (documentation, shared libraries)"
    REQUIRED
)

## Current Markdown documentation for ZIMH.  The inherited Word-format
## source material is retained in the repository but is not installed.
install(DIRECTORY docs/
    TYPE DOC
    COMPONENT runtime_support
    PATTERN "legacy-word" EXCLUDE
)

if (DEFINED ZIMH_EXECUTABLE_PREFIX)
    set(cpack_exe_prefix "${ZIMH_EXECUTABLE_PREFIX}-")
else ()
    set(cpack_exe_prefix "")
endif()

cpack_add_component(altairz80_family
    DISPLAY_NAME "Altair Z80 simulator."
    DESCRIPTION "The Altair Z80 simulator with M68000 support. Simulators: ${cpack_exe_prefix}altairz80"
)
cpack_add_component(att3b2_family
    DISPLAY_NAME "ATT&T 3b2 collection"
    DESCRIPTION "The AT&T 3b2 simulator family. Simulators: ${cpack_exe_prefix}3b2-400, ${cpack_exe_prefix}3b2-700"
)
cpack_add_component(b5500_family
    DISPLAY_NAME "Burroughs 5500"
    DESCRIPTION "The Burroughs 5500 system simulator. Simulators: ${cpack_exe_prefix}b5500"
)
cpack_add_component(cdc1700_family
    DISPLAY_NAME "CDC 1700"
    DESCRIPTION "The Control Data Corporation's systems. Simulators: ${cpack_exe_prefix}cdc1700"
)
cpack_add_component(decpdp_family
    DISPLAY_NAME "DEC PDP family"
    DESCRIPTION "Digital Equipment Corporation PDP systems. Simulators: ${cpack_exe_prefix}pdp1, ${cpack_exe_prefix}pdp15, ${cpack_exe_prefix}pdp4, ${cpack_exe_prefix}pdp6, ${cpack_exe_prefix}pdp7, ${cpack_exe_prefix}pdp8, ${cpack_exe_prefix}pdp9"
)
cpack_add_component(default_family
    DISPLAY_NAME "Default SIMH simulator family."
    DESCRIPTION "The SIMH simulator collection of historical processors and computing systems that do not belong to
any other simulated system family. Simulators: ${cpack_exe_prefix}altair, ${cpack_exe_prefix}besm6, ${cpack_exe_prefix}linc, ${cpack_exe_prefix}ssem, ${cpack_exe_prefix}tt2500, ${cpack_exe_prefix}tx-0"
)
cpack_add_component(dgnova_family
    DISPLAY_NAME "DG Nova and Eclipse"
    DESCRIPTION "Data General NOVA and Eclipse system simulators. Simulators: ${cpack_exe_prefix}eclipse, ${cpack_exe_prefix}nova"
)
cpack_add_component(experimental_family
    DISPLAY_NAME "Experimental (work-in-progress) simulators"
    DESCRIPTION "Experimental or work-in-progress simulators not in the SIMH mainline simulator suite. Simulators: ${cpack_exe_prefix}alpha, ${cpack_exe_prefix}pdq3, ${cpack_exe_prefix}sage"
)
cpack_add_component(gould_family
    DISPLAY_NAME "Gould simulators"
    DESCRIPTION "Gould Systems simulators. Simulators: ${cpack_exe_prefix}sel32"
)
cpack_add_component(grisys_family
    DISPLAY_NAME "GRI Systems GRI-909"
    DESCRIPTION "GRI Systems GRI-909 system simulator. Simulators: ${cpack_exe_prefix}gri"
)
cpack_add_component(honeywell_family
    DISPLAY_NAME "Honeywell H316"
    DESCRIPTION "Honeywell H-316 system simulator. Simulators: ${cpack_exe_prefix}h316"
)
cpack_add_component(hp_family
    DISPLAY_NAME "HP 2100, 3000"
    DESCRIPTION "Hewlett-Packard H2100 and H3000 simulators. Simulators: ${cpack_exe_prefix}hp2100, ${cpack_exe_prefix}hp3000"
)
cpack_add_component(ibm_family
    DISPLAY_NAME "IBM"
    DESCRIPTION "IBM system simulators: i650. Simulators: ${cpack_exe_prefix}i1401, ${cpack_exe_prefix}i1620, ${cpack_exe_prefix}i650, ${cpack_exe_prefix}i701, ${cpack_exe_prefix}i7010, ${cpack_exe_prefix}i704, ${cpack_exe_prefix}i7070, ${cpack_exe_prefix}i7080, ${cpack_exe_prefix}i7090, ${cpack_exe_prefix}i7094, ${cpack_exe_prefix}ibm1130, ${cpack_exe_prefix}s3"
)
cpack_add_component(imlac_family
    DISPLAY_NAME "IMLAC"
    DESCRIPTION "IMLAC system simulators. Simulators: ${cpack_exe_prefix}imlac"
)
cpack_add_component(intel_family
    DISPLAY_NAME "Intel"
    DESCRIPTION "Intel system simulators. Simulators: ${cpack_exe_prefix}intel-mds, ${cpack_exe_prefix}scelbi"
)
cpack_add_component(interdata_family
    DISPLAY_NAME "Interdata"
    DESCRIPTION "Interdata systems simulators. Simulators: ${cpack_exe_prefix}id16, ${cpack_exe_prefix}id32"
)
cpack_add_component(lgp_family
    DISPLAY_NAME "LGP"
    DESCRIPTION "Librascope systems. Simulators: ${cpack_exe_prefix}lgp"
)
cpack_add_component(norsk_family
    DISPLAY_NAME "ND simulators"
    DESCRIPTION "Norsk Data systems simulator family. Simulators: ${cpack_exe_prefix}nd100"
)
cpack_add_component(pdp10_family
    DISPLAY_NAME "DEC PDP-10 collection"
    DESCRIPTION "DEC PDP-10 architecture simulators and variants. Simulators: ${cpack_exe_prefix}pdp10, ${cpack_exe_prefix}pdp10-ka, ${cpack_exe_prefix}pdp10-ki, ${cpack_exe_prefix}pdp10-kl, ${cpack_exe_prefix}pdp10-ks"
)
cpack_add_component(pdp11_family
    DISPLAY_NAME "DEC PDP-11 collection."
    DESCRIPTION "DEC PDP-11 and PDP-11-derived architecture simulators. Simulators: ${cpack_exe_prefix}pdp11, ${cpack_exe_prefix}uc15"
)
cpack_add_component(sds_family
    DISPLAY_NAME "SDS simulators"
    DESCRIPTION "Scientific Data Systems (SDS) systems. Simulators: ${cpack_exe_prefix}sds, ${cpack_exe_prefix}sigma"
)
cpack_add_component(swtp_family
    DISPLAY_NAME "SWTP simulators"
    DESCRIPTION "Southwest Technical Products (SWTP) system simulators. Simulators: ${cpack_exe_prefix}swtp6800mp-a, ${cpack_exe_prefix}swtp6800mp-a2"
)
cpack_add_component(vax_family
    DISPLAY_NAME "DEC VAX simulator collection"
    DESCRIPTION "The Digital Equipment Corporation VAX (plural: VAXen) simulator family. Simulators: ${cpack_exe_prefix}infoserver100, ${cpack_exe_prefix}infoserver1000, ${cpack_exe_prefix}infoserver150vxt, ${cpack_exe_prefix}microvax1, ${cpack_exe_prefix}microvax2, ${cpack_exe_prefix}microvax2000, ${cpack_exe_prefix}microvax3100, ${cpack_exe_prefix}microvax3100e, ${cpack_exe_prefix}microvax3100m80, ${cpack_exe_prefix}microvax3900, ${cpack_exe_prefix}rtvax1000, ${cpack_exe_prefix}vax, ${cpack_exe_prefix}vax730, ${cpack_exe_prefix}vax750, ${cpack_exe_prefix}vax780, ${cpack_exe_prefix}vax8200, ${cpack_exe_prefix}vax8600, ${cpack_exe_prefix}vaxstation3100m30, ${cpack_exe_prefix}vaxstation3100m38, ${cpack_exe_prefix}vaxstation3100m76, ${cpack_exe_prefix}vaxstation4000m60, ${cpack_exe_prefix}vaxstation4000vlc"
)
