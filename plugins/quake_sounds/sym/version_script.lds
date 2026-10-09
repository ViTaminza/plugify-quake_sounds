{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* permissions binding slots (defined by permissions_bindings, filled by the plugify
           module from the permissions plugin's exported methods; the dependency is optional) */
        __permissions_*;

        /* hudmenu binding slots - pisexcompat resolves every displayed menu through
           __hudmenu_GetClientMenuType when the hudmenu plugin is installed */
        __hudmenu_*;

        /* No exported methods: quake_sounds exposes nothing to other plugins. */

    local: *;
};
