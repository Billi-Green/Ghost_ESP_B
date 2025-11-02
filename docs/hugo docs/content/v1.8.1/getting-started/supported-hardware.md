---
title: "Supported Hardware"
description: "Device compatibility matrix for GhostESP features"
weight: 15
toc: true
---

## Overview

GhostESP runs on a variety of ESP32 boards with varying feature support. This compatibility matrix helps you identify which features are available on your device.

## Compatibility Matrix

<style>
  .compat-table {
    --compat-bg: #ffffff;
    --compat-header-bg: #f5f5f5;
    --compat-text: #000000;
    --compat-border: #e0e0e0;
    --compat-hover: #f9f9f9;
    border-radius: 0.5rem;
    max-height: 70vh;
    overflow: auto;
    box-shadow: 0 1px 3px rgba(0,0,0,0.1);
    background: var(--compat-bg);
    position: relative;
    isolation: isolate;
  }
  .compat-table table {
    margin: 0;
    width: 100%;
    border-collapse: separate;
    border-spacing: 0;
    min-width: 720px;
    color: var(--compat-text);
  }
  .compat-table th,
  .compat-table td {
    padding: 0.75rem;
    text-align: center;
    vertical-align: middle;
    border-bottom: 1px solid var(--compat-border);
  }
  .compat-table th:first-child,
  .compat-table td:first-child {
    text-align: left;
    position: sticky;
    left: 0;
    background: var(--compat-bg);
    box-shadow: 1px 0 0 var(--compat-border);
    z-index: 2;
  }
  .compat-table tbody tr:hover { background: var(--compat-hover); }
  .compat-table thead th {
    position: sticky;
    top: 0;
    z-index: 1;
    background: var(--compat-header-bg);
    box-shadow: 0 1px 2px rgba(0,0,0,0.25);
  }
  .compat-table thead th:first-child {
    left: 0;
    z-index: 3;
    background: var(--compat-header-bg);
  }

  :where([data-theme="dark"], html[data-bs-theme="dark"], body[data-bs-theme="dark"], [data-bs-theme="dark"], html.dark, body.dark, .dark-mode, .theme-dark) .compat-table {
    --compat-bg: #1a1a1a;
    --compat-header-bg: #2d2d2d;
    --compat-text: #ffffff;
    --compat-border: #3d3d3d;
    --compat-hover: #252525;
  }
  :where([data-theme="light"], html[data-bs-theme="light"], body[data-bs-theme="light"], [data-bs-theme="light"], html.light, body.light, .light-mode, .theme-light) .compat-table {
    --compat-bg: #ffffff;
    --compat-header-bg: #f5f5f5;
    --compat-text: #000000;
    --compat-border: #e0e0e0;
    --compat-hover: #f9f9f9;
  }

  @media (prefers-color-scheme: dark) {
    :root:not([data-theme]) .compat-table {
      --compat-bg: #1a1a1a;
      --compat-header-bg: #2d2d2d;
      --compat-text: #ffffff;
      --compat-border: #3d3d3d;
      --compat-hover: #252525;
    }
  }
</style>

<div class="compat-table">
  <table>
    <thead>
      <tr>
        <th>Board</th>
        <th>Bluetooth</th>
        <th>NFC (PN532)</th>
        <th>NFC (Chameleon)</th>
        <th>IR TX</th>
        <th>IR RX</th>
        <th>GPS</th>
        <th>Keyboard</th>
        <th>Display</th>
        <th>SD Card</th>
      </tr>
    </thead>
    <tbody>
      <tr><th scope="row">CYD2USB</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">CYDMicroUSB</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">CYDDualUSB</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">CYD2432S028R</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">CYD 2.4″ variants</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">Waveshare 7″</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">Crowtech 7″</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">Sunton 7″</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">Cardputer</th><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">Cardputer ADV</th><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">MarauderV4</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">MarauderV6</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">AwokMini</th><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">Awok V5</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">T-Display S3 Touch</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">S3TWatch</th><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>has 4MB vfs partition</td></tr>
      <tr><th scope="row">TEmbed C1101</th><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">GhostBoard</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">T-Deck</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">JCMK DevBoardPro</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">RabbitLabs Minion</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">Lolin S3 Pro</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">Flipper JCMK GPS</th><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32-S2 (generic)</th><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32-C3 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32-S3 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32-C5 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">ESP32-C6 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td></tr>
    </tbody>
  </table>
</div>
