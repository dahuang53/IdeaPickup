"""Same-origin management and compatible device HTTP adapters.

The factory accepts the task runtime explicitly. Constructing a web application
does not start a worker or permit audio before recovery/configuration checks.
"""
from __future__ import annotations

import json
import hashlib
import anyio
from pathlib import Path
from urllib.parse import quote, unquote, urlsplit

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse, FileResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from starlette.concurrency import run_in_threadpool

from .admin import AdminError, AdminStore
from .identity import IdentityError, IdentityStore, normalize_base_url
from app import RelayError
from stream_service import StreamError, MAX_CHUNK
from config_broker.core import BrokerError


COOKIE = "__Secure-voicememo-admin"
MAX_JSON = 16 * 1024


class WebError(Exception):
    def __init__(self, status, code):
        self.status, self.code = status, code


async def body(request: Request, allowed: set[str], required: set[str] | None = None):
    if request.headers.get("content-type", "").split(";", 1)[0] != "application/json":
        raise WebError(415, "json_required")
    collected = bytearray()
    async for chunk in request.stream():
        collected.extend(chunk)
        if len(collected) > MAX_JSON:
            raise WebError(413, "request_too_large")
    try:
        def unique_pairs(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise ValueError("duplicate key")
                result[key] = value
            return result
        value = json.loads(collected, object_pairs_hook=unique_pairs,
                           parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
    except (ValueError, UnicodeError, RecursionError):
        raise WebError(400, "invalid_json") from None
    if not isinstance(value, dict) or not set(value) <= allowed or not (required or set()) <= set(value):
        raise WebError(400, "invalid_fields")
    return value


def bearer(request):
    authorization = request.headers.get("authorization", "")
    if not authorization.startswith("Bearer ") or len(authorization) > 256:
        raise WebError(401, "unauthorized")
    return authorization[7:]


def create_app(database: Path, *, public_url: str, broker=None, runtime=None):
    canonical = normalize_base_url(public_url)
    parsed = urlsplit(canonical)
    origin = parsed.scheme + "://" + parsed.netloc
    tls = parsed.scheme == "https"
    cookie = COOKIE if tls else "voicememo-admin-lan"
    prefix = parsed.path
    # HTTP/ASGI path is decoded, while redirects and cookie Path are URI text.
    route_prefix = unquote(prefix)
    public_prefix = quote(prefix, safe="/%")
    identity = IdentityStore(database)
    admins = AdminStore(database)
    app = FastAPI(docs_url=None, redoc_url=None, openapi_url=None, root_path=route_prefix)
    app.state.admins, app.state.identity = admins, identity
    app.state.runtime, app.state.broker = runtime, broker

    async def error_handler(request, error):
        return JSONResponse({"error": error.code, "message": "Request could not be completed."}, status_code=error.status)

    for error_class in (AdminError, IdentityError, WebError, RelayError, StreamError):
        app.add_exception_handler(error_class, error_handler)

    @app.exception_handler(BrokerError)
    async def broker_failure(request, error):
        status = 503 if error.code in {"maintenance", "maintenance_conflict", "unavailable"} else 409
        return JSONResponse({"error": error.code, "message": "Configuration service could not complete the request."}, status_code=status)

    @app.middleware("http")
    async def request_boundary(request, next_handler):
        # Liveness/readiness are intentionally available to the private Docker
        # health probe. The reverse proxy must not expose an alternate HTTP UI.
        path = request.url.path
        is_health = path.removeprefix(route_prefix) in ("/health", "/health/live", "/health/ready")
        if not is_health and (request.url.scheme != parsed.scheme or request.headers.get("host") != parsed.netloc):
            return JSONResponse({"error": "https_origin_required" if tls else "service_origin_required"}, status_code=400)
        response = await next_handler(request)
        response.headers.update({
            "Cache-Control": "no-store", "X-Content-Type-Options": "nosniff",
            "X-Frame-Options": "DENY", "Referrer-Policy": "no-referrer",
            "Content-Security-Policy": "default-src 'self'; script-src 'self'; style-src 'self'; "
                                       "object-src 'none'; base-uri 'none'; frame-ancestors 'none'",
        })
        if request.url.scheme == "https":
            response.headers["Strict-Transport-Security"] = "max-age=31536000"
        return response

    def check_origin(request):
        if request.headers.get("origin") != origin:
            raise WebError(403, "origin_denied")

    async def admin(request, *, write=False):
        if write:
            check_origin(request)
        return await run_in_threadpool(admins.authenticate, request.cookies.get(cookie),
                                      csrf=request.headers.get("x-csrf-token"), require_csrf=write)

    def peer(request):
        # Never trust arbitrary X-Forwarded-For supplied directly by a client.
        return request.client.host if request.client else "unknown"

    async def rpc(method, **params):
        if broker is None:
            raise WebError(503, "configuration_broker_unavailable")
        from config_broker.core import BrokerError
        try:
            return await run_in_threadpool(broker.call, method, **params)
        except BrokerError as error:
            # Broker codes are fixed constants, never its raw exception data.
            status = 409 if error.code in {"revision_conflict", "snapshot_limit", "candidate_pending", "maintenance"} else 400
            raise WebError(status, error.code) from None
        except (OSError, TimeoutError):
            raise WebError(503, "configuration_broker_unavailable") from None

    @app.get("/health/live")
    @app.get("/health")
    async def live():
        return {"status": "ok", "service": "voice-memo-relay", "protocol": 2}

    @app.get("/health/ready")
    async def ready():
        state = await run_in_threadpool(runtime.readiness) if runtime else {"ready": False, "reason": "not_configured"}
        return JSONResponse(state, status_code=200 if state["ready"] else 503)

    static = Path(__file__).resolve().parents[1] / "static"
    app.mount("/admin/assets", StaticFiles(directory=static), name="admin-assets")

    @app.get("/")
    @app.get("/admin")
    async def admin_redirect():
        return RedirectResponse(public_prefix + "/admin/", status_code=307)

    @app.get("/admin/")
    async def admin_page(request: Request):
        raw = request.scope.get("raw_path", request.url.path.encode("utf-8")).split(b"?", 1)[0].decode("utf-8")
        canonical = public_prefix + "/admin/"
        # A decoded alias must reach the published URI before cookies are set.
        # A prefix-stripping proxy supplies /admin/ internally, so do not loop.
        if unquote(raw) == route_prefix + "/admin/" and raw != canonical:
            return RedirectResponse(canonical, status_code=307)
        return FileResponse(static / "admin.html", media_type="text/html")

    def tasks():
        if runtime is None:
            raise WebError(503, "not_configured")
        return runtime

    async def result(call, *args, **kwargs):
        value, status = await run_in_threadpool(call, *args, **kwargs)
        return JSONResponse(value, status_code=status)

    @app.post("/v1/memos")
    async def upload_wav(request: Request):
        text = request.headers.get("content-length", "")
        if not text.isascii() or not text.isdigit() or len(text) > 12:
            raise WebError(411, "content_length_required")
        length = int(text)
        service = tasks()
        admission = await run_in_threadpool(service.admit_wav, request.headers, length)
        received = 0
        digest = hashlib.sha256()
        try:
            # Admission allocates a private, bounded receive path before ACK.
            # No request.body()/whole-recording allocation is allowed here.
            with admission.path.open("wb") as output:
                async for chunk in request.stream():
                    received += len(chunk)
                    if received > length:
                        raise WebError(400, "content_length_mismatch")
                    await run_in_threadpool(output.write, chunk)
                    digest.update(chunk)
                if received != length:
                    raise WebError(400, "incomplete_upload")
                await run_in_threadpool(output.flush)
            return await result(service.upload_path, request.headers, admission.path, digest.hexdigest(), admission=admission)
        finally:
            # Disconnect/cancellation must release its receive slot and partial
            # file, even when the request's surrounding cancel scope has fired.
            with anyio.CancelScope(shield=True):
                await run_in_threadpool(service.release_admission, admission)

    @app.get("/v1/memos/{device}/{recording}")
    async def memo_status(device: str, recording: int, request: Request):
        return await result(tasks().memo_status, request.headers.get("authorization"), device, recording)


    @app.post("/v1/memos/{device}/{recording}/cancel")
    async def memo_cancel(device: str, recording: int, request: Request):
        value = await body(request, {"cancel_token"})
        return await result(tasks().cancel_job, request.headers.get("authorization"), device, recording, "wav", **value)

    @app.post("/v1/streams/{device}/{recording}/cancel")
    async def stream_cancel(device: str, recording: int, request: Request):
        await body(request, set())
        return await result(tasks().cancel_job, request.headers.get("authorization"), device, recording, "stream")

    @app.post("/v1/memos/{device}/{recording}/retry")
    async def memo_retry(device: str, recording: int, request: Request):
        value = await body(request, {"cancel_token"})
        return await result(tasks().memo_retry, request.headers.get("authorization"), device, recording, **value)

    @app.post("/v1/streams")
    async def stream_create(request: Request):
        value = await body(request, {"format", "sample_rate", "channels", "frame_ms", "bitrate"})
        return await result(tasks().stream_create, request.headers, value)

    @app.get("/v1/streams/{device}/{recording}")
    async def stream_status(device: str, recording: int, request: Request):
        return await result(tasks().stream_status, request.headers.get("authorization"), device, recording)

    @app.put("/v1/streams/{device}/{recording}/chunks/{sequence}")
    async def stream_put(device: str, recording: int, sequence: int, request: Request):
        service, auth = tasks(), request.headers.get("authorization")
        await run_in_threadpool(service.authorize_stream, auth, device, recording)
        data = bytearray()
        async for chunk in request.stream():
            data.extend(chunk)
            if len(data) > MAX_CHUNK:
                raise WebError(413, "request_too_large")
        return await result(service.stream_put, auth, device, recording, sequence, request.headers, bytes(data))

    @app.post("/v1/streams/{device}/{recording}/finish")
    async def stream_finish(device: str, recording: int, request: Request):
        value = await body(request, {"chunks", "samples", "interrupted", "reason"}, {"chunks", "samples", "interrupted", "reason"})
        return await result(tasks().stream_finish, request.headers.get("authorization"), device, recording, value)

    @app.post("/v1/streams/{device}/{recording}/retry")
    async def stream_retry(device: str, recording: int, request: Request):
        return await result(tasks().stream_retry, request.headers.get("authorization"), device, recording)

    @app.get("/v1/info")
    async def info():
        return await run_in_threadpool(identity.info)

    @app.get("/v1/device-info")
    async def device_info(request: Request):
        return await run_in_threadpool(identity.device_info, request.headers.get("x-device-id", ""), bearer(request))


    @app.post("/v1/enrollments")
    async def enroll(request: Request):
        fields = {"device_id", "pairing_code", "enrollment_id", "candidate_token"}
        value = await body(request, fields, fields)
        return JSONResponse(await run_in_threadpool(identity.create_enrollment, **value), status_code=201)

    @app.get("/v1/enrollments/{transaction}")
    async def enrollment_status(transaction: str, request: Request):
        return await run_in_threadpool(identity.enrollment_status, transaction, bearer(request))


    @app.post("/v1/enrollments/{transaction}/activate")
    async def activate_enrollment(transaction: str, request: Request):
        await body(request, set())
        return await run_in_threadpool(identity.activate_enrollment, transaction, bearer(request))

    @app.post("/v1/enrollments/{transaction}/finalize")
    async def finalize_enrollment(transaction: str, request: Request):
        await body(request, set())
        return await run_in_threadpool(identity.finalize_enrollment, transaction, bearer(request))



    @app.get("/admin/api/session")
    async def session(request: Request):
        try:
            state = await admin(request)
        except AdminError as error:
            if error.status != 401:
                raise
            state = {"authenticated": False}
        return {**state, "initialized": await run_in_threadpool(admins.initialized)}

    @app.post("/admin/api/session/initialize")
    async def initialize(request: Request):
        check_origin(request)
        value = await body(request, {"code", "password"}, {"code", "password"})
        await run_in_threadpool(admins.initialize, value["code"], value["password"], peer=peer(request))
        return {"initialized": True}

    @app.post("/admin/api/session/login")
    async def login(request: Request):
        check_origin(request)
        value = await body(request, {"password"}, {"password"})
        state = await run_in_threadpool(admins.login, value["password"], peer=peer(request))
        response = JSONResponse({"authenticated": True, "csrf": state["csrf"]})
        response.set_cookie(cookie, state["token"], max_age=state["max_age"], path=public_prefix + "/admin",
                            secure=tls, httponly=True, samesite="strict")
        return response

    @app.post("/admin/api/session/logout")
    async def logout(request: Request):
        await admin(request, write=True)
        await run_in_threadpool(admins.logout, request.cookies.get(cookie))
        response = JSONResponse({"authenticated": False})
        response.delete_cookie(cookie, path=public_prefix + "/admin", secure=tls, httponly=True, samesite="strict")
        return response

    @app.get("/admin/api/config")
    async def config_view(request: Request):
        await admin(request)
        return {**await rpc("view"), "public_url": canonical}

    @app.get("/admin/api/status")
    async def deployment_status(request: Request):
        await admin(request)
        from . import __version__
        from .jobs import RUNTIME_SCHEMA_VERSION
        state = await run_in_threadpool(tasks().diagnostics)
        return {**state, "code_version": __version__, "schema_version": RUNTIME_SCHEMA_VERSION,
                "instance_uuid": identity.server_instance_uuid, "public_url": canonical}

    @app.post("/admin/api/config")
    async def config_save(request: Request):
        await admin(request, write=True)
        value = await body(request, {"expected_revision", "patch"}, {"expected_revision", "patch"})
        if not isinstance(value["patch"], dict) or not set(value["patch"]) <= {"qwen_api_key", "siyuan_token"}:
            raise WebError(400, "read_only_config")
        return {**await rpc("save_activate", **value), "public_url": canonical}

    @app.post("/admin/api/config/validate")
    async def config_validate(request: Request):
        await admin(request, write=True)
        value = await body(request, {"patch"}, {"patch"})
        if not isinstance(value["patch"], dict) or not set(value["patch"]) <= {"qwen_api_key", "siyuan_token"}:
            raise WebError(400, "read_only_config")
        return await rpc("validate", **value)



    @app.post("/admin/api/devices/pairing-code")
    async def pairing_code(request: Request):
        await admin(request, write=True)
        value = await body(request, {"ttl_seconds"})
        ttl = value.get("ttl_seconds", 900)
        if type(ttl) is not int or not 60 <= ttl <= 3600:
            raise WebError(400, "invalid_pairing_ttl")
        code, expires = await run_in_threadpool(identity.issue_pairing_code, ttl)
        return {"pairing_code": code, "expires_at": expires}

    @app.get("/admin/api/devices")
    async def devices(request: Request):
        await admin(request)
        try:
            limit = int(request.query_params.get("limit", "50"))
            offset = int(request.query_params.get("offset", "0"))
        except ValueError:
            raise WebError(400, "invalid_pagination") from None
        if not 1 <= limit <= 100 or not 0 <= offset <= 1000000:
            raise WebError(400, "invalid_pagination")
        return await run_in_threadpool(identity.list_devices, limit, offset)

    @app.post("/admin/api/devices/{device}/revoke")
    async def revoke(device: str, request: Request):
        await admin(request, write=True)
        await body(request, set())
        return {"revoked": bool(await run_in_threadpool(identity.revoke_device, device))}


    @app.get("/admin/api/jobs")
    async def jobs(request: Request):
        await admin(request)
        try:
            limit, offset = int(request.query_params.get("limit", "50")), int(request.query_params.get("offset", "0"))
        except ValueError:
            raise WebError(400, "invalid_pagination") from None
        if not 1 <= limit <= 100 or not 0 <= offset <= 1000000:
            raise WebError(400, "invalid_pagination")
        return await run_in_threadpool(tasks().list_jobs, limit, offset)

    @app.post("/admin/api/jobs/{device}/{recording}/retry")
    async def retry_job(device: str, recording: int, request: Request):
        await admin(request, write=True)
        value = await body(request, {"expected_revision"})
        return await result(tasks().retry_job, device, recording, **value)

    @app.post("/admin/api/jobs/{device}/{recording}/reconcile")
    async def reconcile_job(device: str, recording: int, request: Request):
        await admin(request, write=True)
        value = await body(request, {"present"}, {"present"})
        if type(value["present"]) is not bool:
            raise WebError(400, "invalid_present")
        return await result(tasks().reconcile_job, device, recording, value["present"])

    @app.post("/admin/api/config/probe")
    async def probe(request: Request):
        await admin(request, write=True)
        value = await body(request, {"service", "end_to_end"}, {"service", "end_to_end"})
        if value["service"] not in ("qwen", "siyuan", "all") or type(value["end_to_end"]) is not bool:
            raise WebError(400, "invalid_fields")
        return await run_in_threadpool(tasks().probe, value["service"],
                                      mode="end_to_end" if value["end_to_end"] else "tls",
                                      allow_side_effect=value["end_to_end"])

    return app
