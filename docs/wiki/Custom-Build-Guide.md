# Custom Build Using GitHub Actions

This guide will walk you through the process of creating your own custom build of GhostESP using GitHub Actions.

## Prerequisites

- A GitHub account
- Basic understanding of GitHub (forking, repositories)

## Step-by-Step Guide

1. **Fork the Repository**
   - Go to the main GhostESP repository on GitHub
   - Click the "Fork" button in the top-right corner
   - Wait for the forking process to complete

2. **Enable GitHub Actions**
   - Go to your forked repository
   - Click on the "Actions" tab
   - If prompted, click "I understand my workflows, go ahead and enable them"

3. **Trigger the Build**
   - Navigate to the "Actions" tab in your forked repository
   - Select the "Custom Build" workflow from the left sidebar
   - Click "Run workflow" button
   - Choose your desired build options from the dropdown menus (if available)
   - Click "Run workflow" to start the build process

4. **Access Your Build**
   - Once the workflow completes successfully (indicated by a green checkmark)
   - Click on the completed workflow run
   - Scroll down to the "Artifacts" section
   - Download the build artifacts containing your custom firmware

## Build Options

The custom build workflow allows you to configure various options for your build. These may include:

- Target board selection
- Feature toggles
- Debug options
- Custom configurations

## Troubleshooting

If your build fails, check the following:

1. Ensure your fork is up to date with the main repository
2. Review the workflow logs for any error messages
3. Verify that you have selected compatible build options

> **Note**: The build process may take several minutes to complete. You can monitor the progress in the Actions tab.

### Advanced Usage

For advanced users who want to modify the build process:

1. Navigate to `.github/workflows/` in your forked repository
2. Locate the custom build workflow file
3. Edit the workflow file to customize the build process according to your needs
4. Commit your changes and run the workflow

Remember to follow the repository's guidelines and license terms when making modifications.
