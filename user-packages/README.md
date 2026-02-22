# OmScript User Packages

This directory contains the package registry for the OmScript package manager.

## Package Structure

Each package is a subdirectory containing:

```
package-name/
├── package.json    # Package manifest (name, version, description, entry)
└── *.om            # OmScript source files
```

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
4. Submit a pull request

## Using Packages

```bash
# Search available packages
omsc pkg search

# Install a package
omsc pkg install math

# List installed packages
omsc pkg list

# Get package info
omsc pkg info math

# Remove a package
omsc pkg remove math
```

Installed packages are stored in `om_packages/` in the current working directory.
