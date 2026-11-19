//! Offline staging tool only. The signature argument is the decoded minisign text.
use minisign_verify::{PublicKey, Signature};
use std::{env, error::Error, fs};

// Same full public key in the tagged v1.0.1 through v1.0.5 updater configs.
const KEY: &str = "RWRCp61qOcMD668JnPYnEatKfNnVDAKnYZGy2WhpB0mg5/5vJCyPL4p1";

fn main() -> Result<(), Box<dyn Error>> {
    let args: Vec<_> = env::args_os().skip(1).collect();
    if args.len() != 2 {
        return Err("usage: watchman-verify-release installer decoded.minisig".into());
    }
    let key = PublicKey::from_base64(KEY)?;
    let sig = Signature::decode(&fs::read_to_string(&args[1])?)?;
    key.verify(&fs::read(&args[0])?, &sig, true)?;
    Ok(())
}
