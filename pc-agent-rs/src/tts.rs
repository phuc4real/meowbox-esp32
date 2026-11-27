//! `meowbox say`: Windows text-to-speech (WinRT SpeechSynthesizer) to a WAV for the speaker.

use windows::Media::SpeechSynthesis::SpeechSynthesizer;
use windows::Storage::Streams::DataReader;
use windows::core::HSTRING;

/// Voice: the first whose display name + language contains `voice_hint` (case-insensitive), else the first
/// Vietnamese voice, else Windows' default. Returns the WAV bytes.
pub async fn tts_wav(text: &str, voice_hint: Option<&str>) -> windows::core::Result<Vec<u8>> {
    let synth = SpeechSynthesizer::new()?;
    let voices: Vec<_> = SpeechSynthesizer::AllVoices()?.into_iter().collect();
    let key = |v: &windows::Media::SpeechSynthesis::VoiceInformation| {
        format!("{}{}", v.DisplayName().unwrap_or_default(), v.Language().unwrap_or_default()).to_lowercase()
    };
    let by_hint =
        voice_hint.filter(|h| !h.is_empty()).and_then(|h| voices.iter().find(|v| key(v).contains(&h.to_lowercase())));
    let pick = by_hint.or_else(|| {
        voices.iter().find(|v| v.Language().map(|l| l.to_string().to_lowercase().starts_with("vi")).unwrap_or(false))
    });
    if let Some(v) = pick {
        synth.SetVoice(v)?;
    }
    tracing::info!("TTS voice: {}", synth.Voice()?.DisplayName()?);
    let stream = synth.SynthesizeTextToStreamAsync(&HSTRING::from(text))?.await?;
    let size = stream.Size()? as u32;
    let reader = DataReader::CreateDataReader(&stream.GetInputStreamAt(0)?)?;
    let n = reader.LoadAsync(size)?.await?;
    let mut buf = vec![0u8; n as usize];
    reader.ReadBytes(&mut buf)?;
    Ok(buf)
}

#[cfg(test)]
mod tests {
    #[tokio::test]
    async fn makes_a_wav() {
        let wav = super::tts_wav("xin chào", None).await.expect("a TTS voice is installed");
        assert_eq!(&wav[..4], b"RIFF");
        assert!(wav.len() > 1000);
    }
}
