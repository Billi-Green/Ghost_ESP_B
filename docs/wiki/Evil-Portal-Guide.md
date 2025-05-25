# Evil Portal Guide for Beginners

## What is Evil Portal?

An Evil Portal creates a fake WiFi hotspot that looks like a real login page (like those you see at hotels or cafes). When people try to connect, they see a login page you create. This is for educational and authorized testing only!

> **Important**: You need a board with SD card support (like CYD) for Evil Portal to work!

## Quick Start Guide

1. **Check Your Board**
   - You need a board with SD card support (like the yellow CYD board)
   - Not sure? Check the [Board Guide](Board‐Specific‐Guide.md)

2. **Prepare SD Card**
   - Get an SD card (32GB or less works best)
   - Format it to FAT32
   - Create a folder called `portal` on the SD card

3. **Create Your Login Page**
   - Copy the template at the bottom of this guide
   - Save it as `index.html`
   - Put it in the `portal` folder on your SD card

4. **Start Your Portal**
   - Use the command `startportal /portal/index.html "Free WiFi" domainexample` to create your portal!
   - This creates a WiFi network called "Free WiFi"

5. **Test It!**
   - Look for "Free WiFi" in your phone's WiFi list
   - Connect to it
   - You should see a login page
   - If it doesn't work, see troubleshooting below

## Step-by-Step Setup

### Setting Up Your SD Card

1. Get an SD card (32GB or less works best)
2. Format it to FAT32
3. Create a folder called `portal` on the SD card
4. Save the login page template as `index.html` in that folder
5. Put the SD card in your board
6. Run: `startportal /portal/index.html "Free WiFi" domainexample`

## Common Problems & Fixes

### "I don't see the WiFi network"

1. Turn your phone's WiFi off and on
2. Wait 30 seconds - it takes time to start
3. Make sure you typed the command correctly
4. Try restarting your board

### "I see the network but no login page"

1. Make sure you're not connected to any other networks
2. Try opening your browser and going to any website
3. If you see MSN, you're connected to real internet instead of the portal
4. Try typing `http://domainexample.local/login` in your browser

### "The page looks wrong on my phone"

- This is normal - mobile browsers can be tricky
- Try a different phone or computer
- Use the simple template below for best results

## Super Simple Template

Copy this exactly - it works on most devices:

```html
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>WiFi Login</title>
    <style>
        body { 
            font-family: Arial; 
            padding: 20px; 
        }
        input, button {
            width: 100%;
            padding: 10px;
            margin: 5px 0;
        }
        button {
            background: blue;
            color: white;
            border: none;
        }
    </style>
</head>
<body>
    <h2>WiFi Login</h2>
    <form action="/login" method="post">
        <input type="text" name="username" placeholder="Email" required>
        <input type="password" name="password" placeholder="Password" required>
        <button type="submit">Connect</button>
    </form>
</body>
</html>
```

## How to Stop

Just type: `stopportal`

## Important Warnings

- Only use this for learning or authorized testing
- Using this to steal real passwords is illegal
- Get permission before testing on others
- This is for educational purposes only

## Need More Help?

- Join our [Discord](https://discord.gg/5cyNmUMgwh)
- Check if your board is supported: [Board Guide](Board‐Specific‐Guide.md)
- Read about commands: [Commands Guide](Commands.md)

## Creating Custom Portal Pages with SingleFile Extension

### What is SingleFile?

SingleFile is a browser extension that saves web pages as single HTML files, perfect for creating custom portal pages.

### Setup SingleFile

1. Install the extension for your browser:
   - [Chrome Web Store](https://chrome.google.com/webstore/detail/singlefile/mpiodijhokgodhhofbcjdecpffjipkle)
   - [Firefox Add-ons](https://addons.mozilla.org/firefox/addon/single-file)

### Using SingleFile to Create Portal Pages

1. **Find a Login Page You Want to Copy**
   - Go to any login page you want to use as template
   - Make sure it's a simple page that works well on mobile

2. **Save the Page**
   - Click the SingleFile extension icon (usually in top-right)
   - Wait for it to process the page
   - It will automatically download an HTML file

3. **Prepare the File**
   - Rename the downloaded file to `index.html`
   - The file already includes all images and CSS!

4. **Important Changes**
   - Open `index.html` in a text editor
   - Find the `<form>` tag
   - Change the form action to `/login`
   - Make sure form method is `post`
   Example:

   ```html
   <form action="/login" method="post">
   ```

5. **Test the Page**
   - Open the HTML file in your browser first
   - Check if it looks good on mobile too
   - If it looks broken, use the simple template provided above instead

### Tips for SingleFile

- Choose simple login pages - they work better
- Avoid pages with lots of images or animations
- Some sites might not work well - try different ones
- Hotel/cafe portal pages usually work best
