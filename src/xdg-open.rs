use ashpd::desktop::open_uri::{OpenDirectoryRequest, OpenFileRequest};
use clap::Parser;
use std::fs::File;
use std::path::Path;
use url::Url;

#[async_std::main]
async fn main() {
    let args = Cli::parse();
    let uri = input_to_uri(&args.uri);

    let uri = match uri {
        Ok(val) => val,
        Err(val) => {
            eprintln!("Bad input: {val}");
            std::process::exit(2);
        }
    };

    match uri {
        Uri::Uri(val) => {
            open_uri(&val).await.expect("Failed to open URI");
        }
        Uri::File(val) => {
            open_file(&val).await.expect("Failed to open file");
        }
        Uri::Directory(val) => {
            open_directory(&val)
                .await
                .expect("Failed to open directory");
        }
    }
}

fn input_to_uri(input: &str) -> Result<Uri, &str> {
    // Edge case: this could result in a false trigger, but it's
    // so insanely unlikely that we don't handle it here, other than the final error.
    if input.contains("://") {
        Ok(Uri::Uri(input.to_string()))
    } else if Path::new(input).is_file() {
        Ok(Uri::File(input.to_string()))
    } else if File::open(input).is_ok() {
        Ok(Uri::Directory(input.to_string()))
    } else {
        Err(input)
    }
}

#[derive(Debug)]
enum Uri {
    Uri(String),
    File(String),
    Directory(String),
}

async fn open_uri(uristr: &str) -> ashpd::Result<()> {
    let uri = Url::parse(uristr).unwrap();
    OpenFileRequest::default().send_uri(&uri).await?;
    Ok(())
}

async fn open_file(filepath: &str) -> ashpd::Result<()> {
    let file = File::open(filepath).unwrap();
    OpenFileRequest::default().send_file(&file).await?;
    Ok(())
}

async fn open_directory(dirpath: &str) -> ashpd::Result<()> {
    let directory = File::open(dirpath).unwrap();
    OpenDirectoryRequest::default().send(&directory).await?;
    Ok(())
}

#[derive(Parser, Debug)]
#[clap(author, version, about)]
struct Cli {
    // Clap is nice enough to force this to be
    // only one argument for us to handle! Yay!
    uri: String,
}

