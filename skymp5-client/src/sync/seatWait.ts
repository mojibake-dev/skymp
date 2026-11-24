export const SEAT_WAIT_MS = 2000;

export const seated = (_r: { sitState: number; furnitureIsLinked: boolean; unitsFromFurniture: number }): boolean => false;

export const seatVerdict = (_r: { linkedFurniture: boolean; seated: boolean; unitsFromFurniture: number; sinceMs: number }): "wait" | "freeze" => "freeze";
