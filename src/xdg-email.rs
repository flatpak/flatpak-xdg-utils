use ashpd::desktop::email::EmailRequest;
use clap::Parser;
use std::fs::File;

#[async_std::main]
async fn main() {
    let args = Cli::parse();

    let email = Email::from(args);

    let mut emailrequest = EmailRequest::default()
        .addresses(email.to)
        .cc(email.cc)
        .bcc(email.bcc)
        .subject(email.subject.as_deref())
        .body(email.body.as_deref());

    for file in &email.attachments {
        emailrequest.add_attachment(file);
    }

    let _ = emailrequest.send().await.map_err(|err| {
        eprintln!("Failed to send mail: {}", err);
        std::process::exit(4);
    });
}

#[derive(Parser, Debug)]
#[clap(author, version, about)]
struct Cli {
    /// Email addresses of recipients
    to: Vec<String>,
    /// Email addresses of recipients in CC
    #[clap(long)]
    cc: Vec<String>,
    /// Email addresses of recipients in BCC
    #[clap(long)]
    bcc: Vec<String>,
    /// Email subject
    #[clap(long)]
    subject: Option<String>,
    /// Email body
    #[clap(long)]
    body: Option<String>,
    /// Paths of files to attach
    #[clap(long)]
    attach: Vec<String>,
    /// Email content is UTF-8. Nonfunctional
    // D-Bus and Rust want UTF-8 strings, we
    // can't give them non-UTF-8 strings.
    #[clap(long)]
    utf8: bool,
}

#[derive(Debug)]
struct Email {
    to: Vec<String>,
    cc: Vec<String>,
    bcc: Vec<String>,
    subject: Option<String>,
    body: Option<String>,
    attachments: Vec<File>,
}

impl From<Cli> for Email {
    fn from(cli: Cli) -> Self {
        Email {
            to: cli.to,
            cc: cli.cc,
            bcc: cli.bcc,
            subject: cli.subject,
            body: cli.body,
            attachments: convert_paths_to_files(cli.attach),
        }
    }
}

fn convert_paths_to_files(paths: Vec<String>) -> Vec<File> {
    paths
        .into_iter()
        .map(|path| {
            File::open(&path)
                .map_err(|err| {
                    eprintln!("Failed to open file '{}': {}", path, err);
                    std::process::exit(2);
                })
                .unwrap()
        })
        .collect()
}

