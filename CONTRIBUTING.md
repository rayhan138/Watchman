# Contributing to Watchman

Thank you for your interest in improving Watchman.

Watchman is a lightweight Windows network monitor focused on clarity, local data storage, and a small desktop experience.

Please note that this project is released with a [Code of Conduct](./CODE_OF_CONDUCT.md). By participating in this project you agree to abide by its terms.

## Ways to Help

You can help by:

- Reporting bugs
- Suggesting improvements
- Testing Watchman on different Windows versions
- Improving documentation
- Improving UI details
- Helping make network and system information more reliable

## Before You Start

Please keep changes focused. A small pull request that fixes one clear problem is easier to review than a large pull request that changes many things at once.

If you want to make a larger change, open an issue first and describe the idea.

## Development Setup

Install the required tools:

- Node.js
- Rust
- Cargo
- Tauri prerequisites for Windows
- Bun or npm

Clone the repository:

```powershell
git clone https://github.com/rayhan138/Watchman.git
cd Watchman
```

Install dependencies:

```powershell
npm install
```

Run the app locally:

```powershell
npm run tauri:dev
```

## Making Changes

Create a new branch:

```powershell
git checkout -b feature/your-change
```

Make your changes, then check the app locally.

Commit your work:

```powershell
git add .
git commit -m "Describe your change"
```

Push your branch:

```powershell
git push origin feature/your-change
```

Then open a pull request on GitHub.

## Pull Request Guidelines

Please include:

- What changed
- Why the change was needed
- How you tested it
- Screenshots or screen recordings for UI changes

Avoid including:

- Build output
- Local app data
- Personal notes
- API keys, signing keys, passwords, or secrets

## Local Data

Watchman stores traffic history locally in:

```text
%APPDATA%\Watchman\watchman.db
```

Do not commit local app data or generated database files.

