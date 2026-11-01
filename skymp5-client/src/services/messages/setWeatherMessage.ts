import { MsgType } from '../../messages';

// thuum docs/verbs/weather.md (M4): the server's weather for the place this
// game is in (a worldspace), or a release (weather 0) in an interior or a
// worldspace the server has no pool for. At login, when the player enters a
// place, and at each change of the server's pick. Server to client, reliable.
export interface SetWeatherMessage {
  t: MsgType.SetWeather;
  place: number; // the worldspace, or the interior cell a release is for
  weather: number; // the WTHR form id; 0 releases the override
  transition: boolean; // blend into it (true) or cut to it (false)
}
