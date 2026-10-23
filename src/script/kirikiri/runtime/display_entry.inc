LR"DANGO((function(proof,width,line){
    var entries=[];var seen=[];
    for(var depth=0;depth<4;depth++) {
        if(!(line instanceof 'Layer') || !proof(line,0)) return;
        for(var i=0;i<seen.count;i++) if(seen[i]===line) return;
        seen.add(line);
        var font=line.font;
        if(!(font instanceof 'Font') || !proof(font,1) || typeof font.face!='String') return;
        var entry=void;
        if(line.drawText===(global.Layer.drawText incontextof line) && font.getTextWidth===(width incontextof font)) {
            entry=%["line"=>line,"font"=>font,"drawTarget"=>line,"drawName"=>'drawText',"widthTarget"=>font,"widthName"=>'getTextWidth'];
        } else if(typeof line._preRenderFontEx=='Object') {
            var aliases=line._preRenderFontEx;
            if(!isvalid aliases || aliases.owner!==line || aliases.origDrawText!==(global.Layer.drawText incontextof line) || aliases.origGetTextWidth!==(width incontextof font)) return;
            entry=%["line"=>line,"font"=>font,"drawTarget"=>aliases,"drawName"=>'origDrawText',"widthTarget"=>aliases,"widthName"=>'origGetTextWidth'];
        } else return;
        entries.add(entry);
        if(typeof line._divlay=='undefined' || line._divlay===void) return entries;
        var child=line._divlay;
        if(!(child instanceof 'Layer') || child.parent!==line || child.window!==line.window) return;
        line=child;
    }
}))DANGO"
