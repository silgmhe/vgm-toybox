"""H-3 step 1: the sampled decoders stop writing the same guard by hand.

Each replacement keeps the decoder's own condition for "this sample is melodic" and its own rate
expression - only the guard-plus-quantise shape moves into Pitch.h.
"""
import io
import re

BASE = r'h:\CbProject\vgmplay-libvgm\plugin\Source\chips'


def patch(filename, pattern, replacement):
    path = BASE + '\\' + filename
    text = io.open(path, encoding='utf-8', newline='').read().replace('\r\n', '\n')
    new, count = re.subn(pattern, replacement, text, flags=re.S)
    if count != 1:
        print(filename, 'MATCHES', count, '- left alone')
        return
    io.open(path, 'w', encoding='utf-8', newline='').write(new.replace('\n', '\r\n'))
    print(filename, 'patched')


# --- the two chips whose whole pitch is the measured cycle -----------------------------------
patch('Okim6295Decoder.cpp',
      r'const Voice& state = impl->voices\[voice\];\n\t*if \(! state\.melodic \|\| state\.cycleLength <= 0\.0\).*?'
      r'return quantiseFrequency \(rate / state\.cycleLength, finePitch\);',
      'const Voice& state = impl->voices[voice];\n'
      '\t\t\treturn noteFromSampleCycle (rate, state.cycleLength, state.melodic, kOneShotNote, finePitch);')

# --- the six whose pitch is the register's transposition (L1) --------------------------------
patch('Rf5c68Decoder.cpp',
      r'const Channel& voice = impl\.channels\[channel\];\n\t*if \(step == 0 \|\| ! voice\.melodic \|\| voice\.cycleLength <= 0\.0\)'
      r'.*?return quantisePlaybackRate \(impl\.bytesPerSecond \(voice\), impl\.chipRate\(\), finePitch\);',
      'const Channel& voice = impl.channels[channel];\n'
      '\t\t\tconst bool measured = step != 0 && voice.melodic && voice.cycleLength > 0.0;\n'
      '\t\t\treturn noteFromPlaybackRate (impl.bytesPerSecond (voice), impl.chipRate(), measured, 60,\n'
      '\t\t\t\t\t\t\t\t\t finePitch);')

patch('SegaPcmDecoder.cpp',
      r'if \(pitch == 0 \|\| melodic == nullptr \|\| ! \*melodic \|\| cycleLength == nullptr\n\t*\|\| \*cycleLength <= 0\.0\)'
      r'.*?return quantisePlaybackRate \(\(double\) pitch \* bytesPerSecondPerStep, rootRate, finePitch\);',
      'const bool measured = pitch != 0 && melodic != nullptr && *melodic\n'
      '\t\t\t\t\t\t&& cycleLength != nullptr && *cycleLength > 0.0;\n'
      '\t\t\treturn noteFromPlaybackRate ((double) pitch * bytesPerSecondPerStep, rootRate, measured, 60,\n'
      '\t\t\t\t\t\t\t\t\t finePitch);')

patch('Ga20Decoder.cpp',
      r'if \(rate >= 0x100 \|\| melodic == nullptr \|\| ! \*melodic \|\| cycleLength == nullptr\n\t*\|\| \*cycleLength <= 0\.0\)'
      r'.*?return quantisePlaybackRate \(byteClock / \(double\) \(0x100 - rate\), kReferenceRate, finePitch\);',
      'const bool measured = rate < 0x100 && melodic != nullptr && *melodic\n'
      '\t\t\t\t\t\t&& cycleLength != nullptr && *cycleLength > 0.0;\n'
      '\t\t\treturn noteFromPlaybackRate (byteClock / (double) (0x100 - rate), kReferenceRate, measured, 60,\n'
      '\t\t\t\t\t\t\t\t\t finePitch);')

# --- the three with a ROM check: L1 whenever the sample is melodic, or when there is no ROM --
for name, rate_expr in [
        ('QSoundDecoder.cpp', 'const double rate = (fixedRate > 0.0) ? fixedRate\n'
                              '\t\t\t\t\t\t\t\t  : (double) period * unitsPerSecondPerStep;'),
        ('K053260Decoder.cpp', 'const double rate = (double) period * bytesPerSecondPerStep;'),
        ('K054539Decoder.cpp', 'const double rate = (double) period * unitsPerSecondPerStep;')]:
    patch(name,
          r'if \(melodic != nullptr && \*melodic && cycleLength != nullptr && \*cycleLength > 0\.0\)'
          r'.*?return 60;[^\n]*',
          rate_expr + '\n'
          '\t\t\tconst bool measured = (rom == nullptr || rom->empty())\n'
          '\t\t\t\t\t\t|| (melodic != nullptr && *melodic && cycleLength != nullptr && *cycleLength > 0.0);\n'
          '\t\t\treturn noteFromPlaybackRate (rate, rootRate, measured, 60, finePitch);')

print('done')
