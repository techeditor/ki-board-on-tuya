#!/usr/bin/env python3
"""Extract text (and optionally tables) from a PDF using pdfplumber.

Usage: python pdf_extract.py <pdf_path> [max_pages]
"""
import sys
import pdfplumber

def main():
    if len(sys.argv) < 2:
        print("usage: pdf_extract.py <pdf> [max_pages]")
        return 1
    path = sys.argv[1]
    max_pages = int(sys.argv[2]) if len(sys.argv) > 2 else 999
    with pdfplumber.open(path) as pdf:
        n = min(len(pdf.pages), max_pages)
        print(f"=== {path} : {len(pdf.pages)} pages (showing {n}) ===")
        for i in range(n):
            page = pdf.pages[i]
            txt = page.extract_text() or ""
            print(f"\n----- PAGE {i+1} -----")
            print(txt)
    return 0

if __name__ == "__main__":
    sys.exit(main())
