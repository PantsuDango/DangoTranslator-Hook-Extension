LR"DANGO((function(proof){var reference=void;try{
    reference=new Layer(kag,kag.primaryLayer);reference.visible=false;
    if(!proof(reference,3)) {invalidate reference;reference=void;return 'layout=0;reason=native-reference';}
    var nativeWidth=reference.font.getTextWidth;
    var resolve=__DANGO_ENTRY_RESOLVER__;
    var conductors=[kag.mainConductor,kag.extraConductor];
    var parserCount=0;var ownerCount=0;
    for(var i=0;i<conductors.count;i++) {
        var c=conductors[i];
        if(c instanceof 'KAGParser' && proof(c,2) && c.getNextTag===(KAGParser.getNextTag incontextof c)) parserCount++;
        if(c.owner===kag) ownerCount++;
    }
    var pages=[kag.fore,kag.back];var layers=0;var compatible=0;
    for(var i=0;i<pages.count;i++) {
        var messages=pages[i].messages;
        if(messages.count<1 || messages.count>32) {invalidate reference;reference=void;return 'layout=0;reason=layer-count';}
        for(var j=0;j<messages.count;j++) {
            var layer=messages[j];layers++;
            if(resolve(proof,nativeWidth,layer.lineLayer)!==void) compatible++;
        }
    }
    invalidate reference;reference=void;
    return 'layout=1;parsers='+parserCount+';owners='+ownerCount+';layers='+layers+';compatible='+compatible;
}catch(e){if(reference!==void) invalidate reference;return 'layout=0;reason='+e.message;}})(global['__DANGO_NATIVE_CALLBACK__']))DANGO"
