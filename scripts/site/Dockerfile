FROM python:3.6-slim

WORKDIR /app

# Install dependencies
RUN pip install --no-cache-dir htmlmin csscompressor jsmin

# Copy the script and HTML file
COPY html_to_header.py .
COPY ghost_site.html .

# The script will be run with volumes mounted, so we just need python available
# Default command can be overridden when running
CMD ["python3", "html_to_header.py"]

