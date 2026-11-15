// thuum #52 (wire-schema ServedLook, SCHEMA_VERSION 53): one look an actor's
// simulator's script drew on it and has not stopped, served with its create
// word (CreateActor's props.looks). The pair of
// skymp5-server/cpp/messages/ServedLookProp.h, as raceOverlay.ts is
// RaceOverlay.h's
export interface ServedLookProp {
  keep: number;    // 1 EffectShader.Play, 3 VisualEffect.Play, 5 eyes
  form: number;    // the shader's, the effect's or the texture set's
  seconds: number; // left, above 0; -1 until its Stop (and the eyes)
  facing: number;  // VisualEffect.Play's facing object as this engine numbers it, 0 for None and every other kind
  seq: number;     // the relay's sequence it was kept at
}
