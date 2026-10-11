"""Device-only upload authorization. Administrators use the separate web session."""
from app import RelayService


class UploadAuthorization(RelayService):
    def __init__(self, store):
        super().__init__(store, "", "")
