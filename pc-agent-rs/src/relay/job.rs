//! A Windows Job Object per stream: yt-dlp, ffmpeg and anything they start are killed together when the job is
//! dropped (client gone, Quit) or when the agent itself dies (KILL_ON_JOB_CLOSE), so no orphan keeps downloading.

use windows::Win32::Foundation::{CloseHandle, HANDLE};
use windows::Win32::System::JobObjects::{
    AssignProcessToJobObject, CreateJobObjectW, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JobObjectExtendedLimitInformation, SetInformationJobObject,
};

pub struct Job(HANDLE);

// SAFETY: a job handle can be used and closed from any thread.
unsafe impl Send for Job {}
unsafe impl Sync for Job {}

impl Job {
    pub fn new() -> windows::core::Result<Self> {
        // SAFETY: creates an unnamed job and sets its limit flags with a correctly sized struct.
        unsafe {
            let h = CreateJobObjectW(None, None)?;
            let mut info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(
                h,
                JobObjectExtendedLimitInformation,
                &info as *const _ as *const _,
                size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            )?;
            Ok(Self(h))
        }
    }

    /// Put a just-started child in the job (it and its future children die with the job).
    pub fn add(&self, child: &tokio::process::Child) -> windows::core::Result<()> {
        let Some(raw) = child.raw_handle() else { return Ok(()) }; // already exited
        // SAFETY: both handles are valid for the duration of the call.
        unsafe { AssignProcessToJobObject(self.0, HANDLE(raw as *mut _)) }
    }
}

impl Drop for Job {
    fn drop(&mut self) {
        // SAFETY: closing our own handle; with KILL_ON_JOB_CLOSE this ends every process in the job.
        unsafe {
            let _ = CloseHandle(self.0);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn dropping_the_job_kills_the_tree() {
        let job = Job::new().unwrap();
        // cmd starts a child ping; both must die with the job
        let mut child = tokio::process::Command::new("cmd")
            .args(["/c", "ping -n 30 127.0.0.1 >nul"])
            .creation_flags(crate::win::CREATE_NO_WINDOW)
            .spawn()
            .unwrap();
        job.add(&child).unwrap();
        tokio::time::sleep(std::time::Duration::from_millis(300)).await;
        drop(job);
        let st = tokio::time::timeout(std::time::Duration::from_secs(3), child.wait()).await;
        assert!(st.is_ok(), "process still running after the job closed");
    }
}
