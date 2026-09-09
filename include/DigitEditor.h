#pragma once
#include <cstdint>

// Generischer, wiederverwendbarer Ziffern-Editor fuer eine UI mit nur zwei
// Tastern (kurz/lang je Taste) -- siehe main.cpp Settings-Modus fuer die
// konkrete Tasten-Zuordnung (Trigger kurz/lang, Mode kurz/lang). Reiner
// Zustandsautomat: kennt weder Taster noch Display noch die konkrete
// Einstellung, die er gerade bearbeitet.
//
// Bildet sowohl echte mehrstellige Dezimalzahlen ab (jede Ziffer zyklisch
// 0..cycleLen[i]-1, ueblicherweise 10, die fuehrende Stelle ggf. enger
// begrenzt durch den Wertebereich) als auch "einstellige" enum-artige Werte
// (digitCount=1, cycleLen[0]=Anzahl moeglicher Werte) -- fuer Letztere ist
// "die Ziffer" schlicht der Options-Index, kein Dezimal-Digit (darf daher
// auch > 9 reichen).
class DigitEditor {
public:
  static const uint8_t MAX_DIGITS = 5;  // reicht fuer uint16_t (65535)

  // Initialisiert digitCount Ziffern. Bei digitCount>1 wird initialValue
  // dezimal in die Ziffern zerlegt (fuehrende Nullen inklusive, Index 0 =
  // am weitesten links/hoechstwertig). Bei digitCount==1 wird initialValue
  // direkt als einzige "Ziffer" uebernommen (kein Dezimal-Digit). Cursor
  // startet auf Position 0.
  void begin(uint8_t digitCount, const uint8_t* cycleLen, uint32_t initialValue);

  void incrementCurrentDigit();  // (digit+1) % cycleLen[cursor]
  void decrementCurrentDigit();  // (digit+cycleLen[cursor]-1) % cycleLen[cursor]

  // Cursor eine Position weiter/zurueck. Liefert true, wenn der Cursor
  // bereits auf der letzten/ersten Position war (= Bearbeitung fertig) --
  // bewegt sich in diesem Fall NICHT weiter.
  bool advanceDigit();
  bool retreatDigit();

  uint8_t digitCount() const { return digitCount_; }
  uint8_t digitAt(uint8_t i) const { return (i < digitCount_) ? digits_[i] : 0; }
  uint8_t cursor() const { return cursor_; }

  // Ziffern zu einer Dezimalzahl zusammengesetzt (digitCount()==1: digitAt(0)
  // direkt), auf maxValue geclampt -- deckt den Fall ab, dass eine enger
  // begrenzte fuehrende Ziffer allein nicht ausreicht (z.B. 6-9-9-9-9=69999
  // bei einer fuehrenden Ziffer 0..6 fuer einen uint16_t-Wertebereich).
  uint32_t assembledValue(uint32_t maxValue) const;

private:
  uint8_t digitCount_ = 0;
  uint8_t cursor_ = 0;
  uint8_t cycleLen_[MAX_DIGITS] = {0};
  uint8_t digits_[MAX_DIGITS] = {0};
};
