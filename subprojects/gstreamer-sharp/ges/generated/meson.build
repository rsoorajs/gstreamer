generated_sources = [
    'GES/Asset.cs',
    'GES/AssetAddedHandler.cs',
    'GES/AssetLoadingHandler.cs',
    'GES/AssetLoadingReturn.cs',
    'GES/AssetRemovedHandler.cs',
    'GES/AudioSource.cs',
    'GES/AudioTestSource.cs',
    'GES/AudioTrack.cs',
    'GES/AudioTransition.cs',
    'GES/AudioUriSource.cs',
    'GES/BaseEffect.cs',
    'GES/BaseEffectClip.cs',
    'GES/BaseEffectTimeTranslationFunc.cs',
    'GES/BaseTransitionClip.cs',
    'GES/BaseXmlFormatter.cs',
    'GES/ChildAddedHandler.cs',
    'GES/ChildPropertyAddedHandler.cs',
    'GES/ChildPropertyRemovedHandler.cs',
    'GES/ChildRemovedHandler.cs',
    'GES/ChildrenControlMode.cs',
    'GES/Clip.cs',
    'GES/ClipAddedHandler.cs',
    'GES/ClipAsset.cs',
    'GES/ClipRemovedHandler.cs',
    'GES/CommandLineFormatter.cs',
    'GES/Constants.cs',
    'GES/Container.cs',
    'GES/ControlBindingAddedHandler.cs',
    'GES/ControlBindingRemovedHandler.cs',
    'GES/CreateElementForGapFunc.cs',
    'GES/CreateTrackElementFunc.cs',
    'GES/CreateTrackElementsFunc.cs',
    'GES/DeepNotifyHandler.cs',
    'GES/DiscoveredHandler.cs',
    'GES/DiscovererManager.cs',
    'GES/Edge.cs',
    'GES/EditMode.cs',
    'GES/Effect.cs',
    'GES/EffectAsset.cs',
    'GES/EffectClip.cs',
    'GES/Error.cs',
    'GES/ErrorLoadingAssetHandler.cs',
    'GES/ErrorLoadingHandler.cs',
    'GES/ExtractableAdapter.cs',
    'GES/ExtractableCheckId.cs',
    'GES/FillTrackElementFunc.cs',
    'GES/Formatter.cs',
    'GES/FormatterCanLoadURIMethod.cs',
    'GES/FormatterLoadFromURIMethod.cs',
    'GES/FormatterSaveToURIMethod.cs',
    'GES/FrameCompositionMeta.cs',
    'GES/GESSharp.BaseEffectTimeTranslationFuncNative.cs',
    'GES/GESSharp.CreateElementForGapFuncNative.cs',
    'GES/GESSharp.CreateTrackElementFuncNative.cs',
    'GES/GESSharp.CreateTrackElementsFuncNative.cs',
    'GES/GESSharp.ExtractableCheckIdNative.cs',
    'GES/GESSharp.FillTrackElementFuncNative.cs',
    'GES/GESSharp.FormatterCanLoadURIMethodNative.cs',
    'GES/GESSharp.FormatterLoadFromURIMethodNative.cs',
    'GES/GESSharp.FormatterSaveToURIMethodNative.cs',
    'GES/GESSharp.MetaForeachFuncNative.cs',
    'GES/Global.cs',
    'GES/Group.cs',
    'GES/GroupAddedHandler.cs',
    'GES/IExtractable.cs',
    'GES/IMetaContainer.cs',
    'GES/ImageSource.cs',
    'GES/Layer.cs',
    'GES/LayerAddedHandler.cs',
    'GES/LayerRemovedHandler.cs',
    'GES/LoadSerializedInfoHandler.cs',
    'GES/LoadedHandler.cs',
    'GES/LoadingHandler.cs',
    'GES/Marker.cs',
    'GES/MarkerAddedHandler.cs',
    'GES/MarkerFlags.cs',
    'GES/MarkerList.cs',
    'GES/MarkerMovedHandler.cs',
    'GES/MarkerRemovedHandler.cs',
    'GES/MetaContainerAdapter.cs',
    'GES/MetaFlag.cs',
    'GES/MetaForeachFunc.cs',
    'GES/MissingUriHandler.cs',
    'GES/MultiFileSource.cs',
    'GES/NotifyMetaHandler.cs',
    'GES/Operation.cs',
    'GES/OperationClip.cs',
    'GES/OverlayClip.cs',
    'GES/Pipeline.cs',
    'GES/PipelineFlags.cs',
    'GES/PitiviFormatter.cs',
    'GES/Project.cs',
    'GES/SelectElementTrackHandler.cs',
    'GES/SnappingEndedHandler.cs',
    'GES/SnappingStartedHandler.cs',
    'GES/Source.cs',
    'GES/SourceClip.cs',
    'GES/SourceClipAsset.cs',
    'GES/SourceSetupHandler.cs',
    'GES/TestClip.cs',
    'GES/TextHAlign.cs',
    'GES/TextOverlay.cs',
    'GES/TextOverlayClip.cs',
    'GES/TextVAlign.cs',
    'GES/Timeline.cs',
    'GES/TimelineElement.cs',
    'GES/TitleClip.cs',
    'GES/TitleSource.cs',
    'GES/Track.cs',
    'GES/TrackAddedHandler.cs',
    'GES/TrackElement.cs',
    'GES/TrackElementAddedHandler.cs',
    'GES/TrackElementAsset.cs',
    'GES/TrackElementRemovedHandler.cs',
    'GES/TrackRemovedHandler.cs',
    'GES/TrackType.cs',
    'GES/Transition.cs',
    'GES/TransitionClip.cs',
    'GES/UriClip.cs',
    'GES/UriClipAsset.cs',
    'GES/UriSource.cs',
    'GES/UriSourceAsset.cs',
    'GES/VideoSource.cs',
    'GES/VideoStandardTransitionType.cs',
    'GES/VideoTestPattern.cs',
    'GES/VideoTestSource.cs',
    'GES/VideoTrack.cs',
    'GES/VideoTransition.cs',
    'GES/VideoUriSource.cs',
    'GES/XmlFormatter.cs',
    'GLib/GLibSharp.AsyncReadyCallbackNative.cs',
    'GtkSharp/ObjectManager.cs',
]

run_target('update_ges_code',
    command: [
        generate_api,
        '--api-raw', raw_api_fname,
        '--gapi-fixup', gapi_fixup,
        '--metadata', metadata,
        '--gapi-codegen', gapi_codegen,
        '--extra-includes=' + glib_api_includes,
        '--extra-includes=' + gio_api_includes,
        '--extra-includes=' + gst_api_includes,
        '--out', meson.current_source_dir(),
        '--files', ';'.join(generated_sources),
        '--assembly-name', pkg,
        '--abi-includes', 'ges/ges.h',
        '--abi-cs-usings', 'Gst,Gst.Video,Gst.Sdp,Gst.Tags,Gst.Rtsp,Gst.PbUtils,Gst.Net,Gst.FFT,Gst.Controller,Gst.Base,Gst.Audio,Gst.App,GES',
    ],
)

ges_generate_files = files(generated_sources)

c_abi = files(pkg + '-abi.c')
cs_abi = files(pkg + '-abi.cs')
gapis += [join_paths(meson.current_build_dir(), pkg + '-api.xml')]
gapis_deps = []
