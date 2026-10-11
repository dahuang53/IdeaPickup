FROM python:3.11.16-slim-bookworm@sha256:a36c24f9cbdf4fd0f52d67f0823eeac19c2028c637cecc392d97f980d4fec56b AS runtime-base

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    TMPDIR=/var/lib/voice-memo-work
WORKDIR /opt/voice-memo/server-relay
RUN apt-get update \
 && apt-get install --yes --no-install-recommends --no-upgrade libopus0=1.3.1-3 \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --gid 10001 relay \
 && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin relay \
 && mkdir -p /usr/share/voice-memo \
 && dpkg-query -W -f='${Package}\t${Version}\n' > /usr/share/voice-memo/base-packages.txt
COPY server-relay/requirements.lock.txt server-relay/container-base.lock.json ./
RUN python -m pip --isolated install --no-cache-dir --require-hashes -r requirements.lock.txt
COPY server-relay/app.py server-relay/stream_service.py ./
COPY server-relay/relay/*.py ./relay/
COPY server-relay/config_broker/*.py ./config_broker/
COPY server-relay/static/*.html server-relay/static/*.css server-relay/static/*.js ./static/
COPY deploy/init-volumes.py deploy/healthcheck.py deploy/storage-check.py /opt/voice-memo/deploy/
COPY server-relay/bin/relay-admin /usr/local/bin/relay-admin
COPY LICENSE THIRD_PARTY_NOTICES.md /usr/share/voice-memo/
COPY licenses/ /usr/share/voice-memo/licenses/
RUN chmod 0555 /usr/local/bin/relay-admin
USER 10001:10001
EXPOSE 8765
STOPSIGNAL SIGTERM
ENTRYPOINT ["python", "-m", "relay"]
CMD ["serve", "--host", "0.0.0.0", "--port", "8765", "--workers", "1"]

# Isolated verification only; never use this target as the deployment image.
FROM runtime-base AS verification
COPY server-relay/test_config_broker.py server-relay/test_support.py ./
USER 0:10001
ENTRYPOINT ["python", "-m", "unittest"]
CMD ["-v", "test_config_broker"]

# Default build target is always the non-root production image.
FROM runtime-base AS runtime
