# Web UI pack (index.html, CSS, JS, strings, fonts, theme previews) built by web/build.py and embedded in the app,
# so the UI is updated together with the firmware (OTA A/B + rollback). See docs/firmware_v0.2.0.md, 3.1.
idf_component_register(
    SRCS "webui.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "web.pack"
)
