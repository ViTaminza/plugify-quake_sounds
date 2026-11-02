{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* permissions binding slots (defined by permissions_bindings; the dependency is optional,
           so a host without the permissions plugin leaves them null) */
        __permissions_*;

        /* optional hudmenu binding slot: the player's own "!menu" choice (GetClientMenuType) */
        __hudmenu_*;

        /* No exported methods: quake_sounds exposes nothing to other plugins. */

    local: *;
};
