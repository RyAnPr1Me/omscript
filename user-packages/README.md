# OmScript User Packages

This directory is the remote package registry for the OmScript package manager.
Packages listed here are downloaded on demand when users run `omsc pkg install`.

## Package Structure

Each package is a subdirectory containing:

```
package-name/
├── package.json    # Package manifest (name, version, description, entry)
└── *.om            # OmScript source files
```

The top-level `index.json` lists all available packages and must be updated
when packages are added or removed.

## Package Manifest (package.json)

```json
{
  "name": "package-name",
  "version": "1.0.0",
  "description": "A brief description of the package",
  "entry": "main.om",
  "files": ["main.om", "helpers.om"]
}
```

## Creating a Package

1. Create a new directory under `user-packages/` with your package name
2. Add a `package.json` manifest
3. Add your `.om` source files
4. Add the package entry to `index.json`
5. Submit a pull request

## Using Packages

```bash
# Search available packages (downloads index from GitHub)
omsc pkg search

# Install a package (downloads files from GitHub)
omsc pkg install math

# List installed packages (local only)
omsc pkg list

# Get package info
omsc pkg info math

# Remove a package
omsc pkg remove math
```

Installed packages are stored in `om_packages/` in the current working directory.

## Custom Registry

Set the `OMSC_REGISTRY_URL` environment variable to use a different package
registry (e.g. a local server or a fork):

```bash
export OMSC_REGISTRY_URL="https://example.com/my-packages"
omsc pkg search
```
