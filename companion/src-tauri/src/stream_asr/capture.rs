//! PC microphone capture (default input device) -> 16 kHz mono PCM chunks.
//! Windows uses cpal (WASAPI); other hosts have no capture (the app ships on
//! Windows only, and tests feed recorded audio instead).

use super::AudioFeeder;

/// A running capture; stops when dropped or on `stop`.
pub struct Capture {
    #[cfg(target_os = "windows")]
    stop: Option<std::sync::mpsc::Sender<()>>,
}

impl Capture {
    pub fn stop(&mut self) {
        #[cfg(target_os = "windows")]
        {
            self.stop = None;
        }
    }
}

impl Drop for Capture {
    fn drop(&mut self) {
        self.stop();
    }
}

/// Start recording into `feeder`. Returns the device name.
#[cfg(target_os = "windows")]
pub fn start(feeder: AudioFeeder) -> Result<(Capture, String), String> {
    use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
    use std::sync::mpsc;

    let (ready_tx, ready_rx) = mpsc::channel::<Result<String, String>>();
    let (stop_tx, stop_rx) = mpsc::channel::<()>();
    // cpal streams are not Send: build, run and drop the stream on one thread.
    std::thread::Builder::new()
        .name("pc-asr-mic".into())
        .spawn(move || {
            let built = (|| -> Result<(cpal::Stream, String), String> {
                let host = cpal::default_host();
                let device = host.default_input_device().ok_or("no microphone found")?;
                let name = device.description().map(|d| d.name().to_string()).unwrap_or_else(|_| "microphone".into());
                let config = device.default_input_config().map_err(|e| format!("microphone config: {e}"))?;
                let rate = config.sample_rate();
                let channels = config.channels();
                let format = config.sample_format();
                let stream_config: cpal::StreamConfig = config.into();
                let mut resampler = super::Resampler::new(rate, channels);
                let err = |e: cpal::Error| crate::diagnostics::record("WARN", "pc-asr", &format!("microphone: {e}"));
                let stream = match format {
                    cpal::SampleFormat::F32 => device.build_input_stream(
                        stream_config.clone(),
                        move |data: &[f32], _: &cpal::InputCallbackInfo| {
                            let mut out = Vec::with_capacity(data.len() / 2);
                            resampler.push(data, &mut out);
                            if !out.is_empty() {
                                feeder.push(out);
                            }
                        },
                        err,
                        None,
                    ),
                    cpal::SampleFormat::I16 => device.build_input_stream(
                        stream_config.clone(),
                        move |data: &[i16], _: &cpal::InputCallbackInfo| {
                            let f: Vec<f32> = data.iter().map(|&s| s as f32 / 32768.0).collect();
                            let mut out = Vec::with_capacity(f.len() / 2);
                            resampler.push(&f, &mut out);
                            if !out.is_empty() {
                                feeder.push(out);
                            }
                        },
                        err,
                        None,
                    ),
                    other => return Err(format!("unsupported microphone sample format {other:?}")),
                }
                .map_err(|e| format!("open microphone: {e}"))?;
                stream.play().map_err(|e| format!("start microphone: {e}"))?;
                Ok((stream, name))
            })();
            match built {
                Ok((stream, name)) => {
                    let _ = ready_tx.send(Ok(name));
                    // Runs until the Capture is dropped (sender gone).
                    let _ = stop_rx.recv();
                    drop(stream);
                }
                Err(e) => {
                    let _ = ready_tx.send(Err(e));
                }
            }
        })
        .map_err(|e| format!("spawn microphone thread: {e}"))?;
    let name = ready_rx
        .recv_timeout(std::time::Duration::from_secs(5))
        .map_err(|_| "microphone did not start".to_string())??;
    Ok((Capture { stop: Some(stop_tx) }, name))
}

#[cfg(not(target_os = "windows"))]
pub fn start(_feeder: AudioFeeder) -> Result<(Capture, String), String> {
    Err("microphone capture is only built for Windows".into())
}
