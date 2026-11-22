// The commit a client bundle was built from. Its build writes a marker into
// the bundle as a literal, "skymp5-client-commit:<sha>" with "-dirty" after
// the sha when the client's tree differed from that commit, so a tool can
// read the commit from the file and the client can say it at its start.

export const BUILD_MARKER = "skymp5-client-commit:";

export interface BuildCommit {
    sha: string;
    dirty: boolean;
}

// the marker's commit, or undefined when it names none (a build with no git)
export function commitOf(marker: string | undefined): BuildCommit | undefined {
    return undefined;
}

// the client's line at its start
export function buildLine(marker: string | undefined): string {
    const c = commitOf(marker);
    if (!c) {
        return "skymp5-client of no recorded commit";
    }
    return c.dirty ? `skymp5-client ${c.sha}, built from a tree that differs from it` : `skymp5-client ${c.sha}`;
}
