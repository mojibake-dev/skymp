import { Settings } from "./settings";
import * as crc32 from "crc-32";
import * as path from "path";
import * as fs from "fs";

interface ManifestModEntry {
  filename: string;
  crc32: number;
  size: number;
  // a plugin the engine loads as light: numbered apart from full plugins
  // (thuum docs/verbs/light-plugins.md); absent on archives
  light?: boolean;
}

// TES4's record flags sit at bytes 8 to 11 of a plugin; 1 << 9 (kSmallFile,
// CommonLibSSE-NG include/RE/T/TESFile.h:48) makes it light, as an .esl
// extension does
const isLightPlugin = (espmName: string, buf: Uint8Array): boolean => {
  if (espmName.toLowerCase().endsWith(".esl")) {
    return true;
  }
  if (buf.length < 12) {
    return false;
  }
  const flags = buf[8] | (buf[9] << 8) | (buf[10] << 16) | (buf[11] << 24);
  return (flags & (1 << 9)) !== 0;
};

interface Manifest {
  versionMajor: number;
  mods: Array<ManifestModEntry>;
  loadOrder: Array<string>;
}

const getBsaNameByEspmName = (espmName: string) => {
  if (espmName.endsWith(".esp") || espmName.endsWith(".esm") || espmName.endsWith(".esl")) {
    const nameNoExt = espmName.split(".").slice(0, -1).join(".");
    return nameNoExt + ".bsa";
  }
  throw new Error(`'${espmName}' is not a valid esp or esm name`);
};

export const generateManifest = (settings: Settings): void => {
  const manifest: Manifest = {
    mods: [],
    versionMajor: 1,
    loadOrder: settings.loadOrder.map(x => path.basename(x)),
  };

  settings.loadOrder.forEach((loadOrderElement) => {
    const espmName = path.isAbsolute(loadOrderElement)
      ? path.basename(loadOrderElement)
      : loadOrderElement;

    const espmPath = path.isAbsolute(loadOrderElement)
      ? loadOrderElement
      : path.join(settings.dataDir, espmName);

    const buf: Uint8Array = fs.readFileSync(espmPath);
    manifest.mods.push({
      crc32: crc32.buf(buf),
      filename: espmName,
      size: buf.length,
      light: isLightPlugin(espmName, buf),
    });

    const bsaName = getBsaNameByEspmName(espmName);
    const bsaPath = path.join(settings.dataDir, bsaName);
    if (fs.existsSync(bsaPath)) {
      const buf: Uint8Array = fs.readFileSync(bsaPath);
      manifest.mods.push({
        crc32: crc32.buf(buf),
        filename: bsaName,
        size: buf.length,
      });
    }
  });

  const manifestPath = path.join(settings.dataDir, "manifest.json");
  fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 4));
};
