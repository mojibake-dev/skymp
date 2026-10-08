export interface Mod {
    filename: string;
    size: number;
    crc32: number;
    // thuum docs/verbs/light-plugins.md: a light plugin, numbered apart from
    // full ones; absent from an older server (every plugin full)
    light?: boolean;
};

export interface ServerManifest {
    versionMajor: number;
    mods: Mod[];
    loadOrder: string[];
};
