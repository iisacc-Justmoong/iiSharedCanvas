include_guard(GLOBAL)

# Runtime packages contain public resource inputs, never local recovery,
# caches, or reserved per-run test workspaces.
function(iisc_install_resources source destination)
    install(DIRECTORY "${source}/" DESTINATION "${destination}"
        PATTERN ".iisacc-*" EXCLUDE
        PATTERN ".recovery" EXCLUDE
        PATTERN ".state" EXCLUDE)
endfunction()
