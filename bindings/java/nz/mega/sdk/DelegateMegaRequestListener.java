package nz.mega.sdk;

/**
 * Interface to receive information about requests.
 * <p>
 * All requests allows to pass a pointer to an implementation of this interface in the last parameter.
 * You can also get information about all requests using MegaApi.addRequestListener().
 * MegaListener objects can also receive information about requests.
 * This interface uses MegaRequest objects to provide information about requests.
 * Please note that not all fields of MegaRequest objects are valid for all requests.
 * See the documentation about each request to know which fields contain useful information for each one.
 */
class DelegateMegaRequestListener extends MegaRequestListener {

    MegaApiJava megaApi;
    MegaRequestListenerInterface listener;
    boolean singleListener;

    DelegateMegaRequestListener(MegaApiJava megaApi, MegaRequestListenerInterface listener, boolean singleListener) {
        this.megaApi = megaApi;
        this.listener = listener;
        this.singleListener = singleListener;
    }

    MegaRequestListenerInterface getUserListener() {
        return listener;
    }

    /**
     * This function is called when a request is about to start being processed.
     * <p>
     * The SDK retains the ownership of the request parameter. Do not use it after this functions returns.
     * The api object is the one created by the application, it will be valid until the application deletes it.
     *
     * @param api
     *            API that started the request
     * @param request
     *            Information about the request
     */
    @Override
    public void onRequestStart(MegaApi api, MegaRequest request) {
        if (listener != null) {
            final MegaRequest megaRequest = request.copy();
            megaApi.runCallback(new Runnable() {
                public void run() {
                    listener.onRequestStart(megaApi, megaRequest);
                }
            });
        }
    }

    /**
     * This function is called to get details about the progress of a request.
     * <p>
     * Currently, this callback is only used for fetchNodes requests (MegaRequest.TYPE_FETCH_NODES).
     * The SDK retains the ownership of the request parameter. Do not use it after this function returns.
     * The api object is the one created by the application, it will be valid until the application deletes it.
     *
     * @param api
     *            API that started the request
     * @param request
     *            Information about the request
     */
    @Override
    public void onRequestUpdate(MegaApi api, MegaRequest request) {
        if (listener != null) {
            final MegaRequest megaRequest = request.copy();
            megaApi.runCallback(new Runnable() {
                public void run() {
                    listener.onRequestUpdate(megaApi, megaRequest);
                }
            });
        }
    }

    /**
     * This function is called when a request has finished.
     * <p>
     * There will be no further callbacks related to this request.
     * The MegaError parameter provides the result of the request.
     * If the request completed without problems, the error code will be API_OK. The SDK retains the ownership
     * of the request and error parameters. Do not use them after this functions returns.
     * The api object is the one created by the application, it will be valid until the application deletes it.
     *
     * @param api
     *            API that started the request
     * @param request
     *            Information about the request
     * @param e
     *            Error Information
     */
    @Override
    public void onRequestFinish(MegaApi api, MegaRequest request, MegaError e) {
        if (listener != null) {
            final MegaRequest megaRequest = request.copy();
            final MegaError megaError = e.copy();
            megaApi.runCallback(new Runnable() {
                public void run() {
                    listener.onRequestFinish(megaApi, megaRequest, megaError);
                }
            });
        }
        if (singleListener) {
            megaApi.privateFreeRequestListener(this);
        }
    }

    /**
     * This function is called when there is a temporary error processing a request.
     * <p>
     * The request continues after this callback, so expect more MegaRequestListener.onRequestTemporaryError
     * or a MegaRequestListener.onRequestFinish callback. The SDK retains the ownership of the request and
     * error parameters. Do not use them after this functions returns.
     * The api object is the one created by the application, it will be valid until the application deletes it.
     *
     * @param api
     *            API that started the request
     * @param request
     *            Information about the request
     * @param e
     *            Error Information
     */
    @Override
    public void onRequestTemporaryError(MegaApi api, MegaRequest request, MegaError e) {
        if (listener != null) {
            final MegaRequest megaRequest = request.copy();
            final MegaError megaError = e.copy();
            megaApi.runCallback(new Runnable() {
                public void run() {
                    listener.onRequestTemporaryError(megaApi, megaRequest, megaError);
                }
            });
        }
    }
}
